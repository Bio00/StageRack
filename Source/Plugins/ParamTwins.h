#pragma once

#include <JuceHeader.h>

namespace stagerack::paramtwins
{

/*  Many plugins have two parameters for one knob: a free one and a tempo-synced one, plus a
    switch ("Repeat Rate Left" in ms, "Repeat Rate Left Sync" in 1/8, 1/4.t... and "Rates Sync"
    in Arturia TAPE-201; "LFO Rate" / "LFO Rate Sync" / "LFO Sync"). With the switch on, the plugin
    ignores the free parameter: a control mapped to it would move nothing. These helpers find the
    pair and the switch by name, so the rack and the MIDI mappings always drive the one in use. */

inline juce::StringArray words (const juce::String& name)
{
    juce::StringArray w;
    juce::String cur;
    for (auto c : name.toLowerCase() + " ")
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c)) { cur += juce::String::charToString (c); continue; }
        if (cur.isNotEmpty())
        {
            if (cur.length() > 3 && cur.endsWithChar ('s') && ! cur.endsWith ("ss")) cur = cur.dropLastCharacters (1);   // "Rates" = "Rate"
            w.add (cur);
        }
        cur.clear();
    }
    return w;
}

inline bool isSyncWord (const juce::String& w)   { return w == "sync" || w == "synced" || w == "synchro"; }

inline bool hasSyncWord (const juce::StringArray& w)
{
    for (auto& s : w) if (isSyncWord (s)) return true;
    return false;
}

inline juce::StringArray withoutSync (const juce::StringArray& w)
{
    juce::StringArray out;
    for (auto& s : w) if (! isSyncWord (s)) out.add (s);
    return out;
}

inline bool isToggle (const juce::AudioProcessorParameter& p)
{
    return p.isBoolean() || (p.isDiscrete() && p.getNumSteps() == 2);
}

struct Pair
{
    juce::AudioProcessorParameter* free = nullptr;
    juce::AudioProcessorParameter* synced = nullptr;
    juce::AudioProcessorParameter* sw = nullptr;   // on = synced in use
};

/** The free/synced pair this parameter belongs to (all null if it has no twin or no switch). */
inline Pair findPair (const juce::Array<juce::AudioProcessorParameter*>& params, const juce::AudioProcessorParameter* prm)
{
    Pair pair;
    if (prm == nullptr || isToggle (*prm))
        return pair;

    const auto w = words (prm->getName (64));
    const auto base = withoutSync (w);
    const bool isSynced = hasSyncWord (w);
    if (base.isEmpty())
        return pair;

    for (auto* q : params)
    {
        if (q == prm || isToggle (*q))
            continue;
        const auto qw = words (q->getName (64));
        if (hasSyncWord (qw) != isSynced && withoutSync (qw) == base)
        {
            pair.free = isSynced ? q : const_cast<juce::AudioProcessorParameter*> (prm);
            pair.synced = isSynced ? const_cast<juce::AudioProcessorParameter*> (prm) : q;
            break;
        }
    }
    if (pair.free == nullptr)
        return {};

    // the switch: an on/off "... Sync" that shares the most words with the knob (first word counts double)
    int best = 0, ties = 0, switches = 0;
    juce::AudioProcessorParameter* only = nullptr;
    for (auto* q : params)
    {
        if (! isToggle (*q))
            continue;
        const auto qw = words (q->getName (64));
        if (! hasSyncWord (qw))
            continue;
        ++switches;
        only = q;
        int score = 0;
        for (auto& s : withoutSync (qw))
            if (base.contains (s))
                score += base[0] == s ? 2 : 1;
        if (score > best)      { best = score; ties = 1; pair.sw = q; }
        else if (score == best && score > 0) ++ties;
    }
    if (best == 0 && switches == 1)
        pair.sw = only;            // a single sync switch in the plugin
    else if (best == 0 || ties > 1)
        pair.sw = nullptr;         // can't tell: leave it alone

    if (pair.sw == nullptr)
        return {};
    return pair;
}

/** The parameter of the pair that the plugin uses now (the parameter itself if it has no twin). */
inline juce::AudioProcessorParameter* active (const juce::Array<juce::AudioProcessorParameter*>& params, juce::AudioProcessorParameter* prm)
{
    const auto pair = findPair (params, prm);
    if (pair.sw == nullptr)
        return prm;
    return pair.sw->getValue() >= 0.5f ? pair.synced : pair.free;
}

/** The free one of a pair: what a mapping or a rack row is stored as (both twins = one control). */
inline juce::AudioProcessorParameter* canonical (const juce::Array<juce::AudioProcessorParameter*>& params, juce::AudioProcessorParameter* prm)
{
    const auto pair = findPair (params, prm);
    return pair.sw != nullptr ? pair.free : prm;
}

} // namespace stagerack::paramtwins
