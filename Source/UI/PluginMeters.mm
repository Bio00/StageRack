#import <AudioToolbox/AudioToolbox.h>
#include "PluginMeters.h"

#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

namespace stagerack
{

static juce::StringArray wordsOf (const juce::String& name)
{
    return juce::StringArray::fromTokens (name.toLowerCase().replaceCharacters ("_-/()[].:", "         "), " ", "");
}

/** GUI state that some plugins (Waves) publish as meters: markers, LED segments, sliders... */
static bool isJunk (const juce::String& name)
{
    const auto trimmed = name.trim();

    if (trimmed.isEmpty())
        return true;

    // indexed elements: "Input_3", "Release_0"
    if (const auto us = trimmed.lastIndexOfChar ('_'); us > 0 && trimmed.substring (us + 1).containsOnly ("0123456789"))
        return true;

    const auto words = wordsOf (trimmed);

    for (auto* k : { "dummy", "marker", "slider", "graph", "led", "clip", "knob", "button", "display", "mode",
                     "program", "preset", "bypass", "attack", "release", "threshold", "ratio",
                     "scale", "target", "range", "type", "style", "speed", "hold", "view", "show", "select", "time" })
        if (words.contains (k))
            return true;

    return false;
}

static bool looksLikeMeter (const juce::AudioProcessorParameter& p)
{
    if (p.isBoolean() || (p.isDiscrete() && p.getNumSteps() <= 2))
        return false;

    const auto name = p.getName (64);

    if (isJunk (name))
        return false;

    const auto words = wordsOf (name);

    for (auto* k : { "meter", "metering", "level", "peak", "rms", "lufs", "reduction", "gr", "activity", "vu",
                     "input", "output", "in", "out", "inl", "inr", "outl", "outr", "correlation", "envelope",
                     "amplitude", "energy", "needle" })
        if (words.contains (k))
            return true;

    return false;
}

static void classify (PluginMeter& m)
{
    const auto words = wordsOf (m.name);
    m.gainReduction = words.contains ("reduction") || words.contains ("gr") || words.contains ("energy")
                   || words.contains ("compression") || words.contains ("comp") || words.contains ("needle");
}

//==============================================================================
static juce::String auName (const AudioUnitParameterInfo& info)
{
    if ((info.flags & kAudioUnitParameterFlag_HasCFNameString) != 0 && info.cfNameString != nullptr)
        return juce::String::fromCFString (info.cfNameString);

    return juce::String (info.name, sizeof (info.name));
}

static juce::Array<PluginMeter> auMeters (AudioUnit au)
{
    juce::Array<PluginMeter> result;
    UInt32 size = 0;

    if (AudioUnitGetPropertyInfo (au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, nullptr) != noErr || size == 0)
        return result;

    std::vector<AudioUnitParameterID> ids (size / sizeof (AudioUnitParameterID));

    if (AudioUnitGetProperty (au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size) != noErr)
        return result;

    for (auto id : ids)
    {
        AudioUnitParameterInfo info {};
        UInt32 infoSize = sizeof (info);

        if (AudioUnitGetProperty (au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &infoSize) != noErr)
            continue;

        // meters: flagged as such, or readable but not writable (Apple's own AUs do this)
        const bool meter = (info.flags & kAudioUnitParameterFlag_MeterReadOnly) != 0
                        || ((info.flags & kAudioUnitParameterFlag_IsReadable) != 0
                            && (info.flags & kAudioUnitParameterFlag_IsWritable) == 0);

        const auto name = auName (info);

        if ((info.flags & kAudioUnitParameterFlag_CFNameRelease) != 0 && info.cfNameString != nullptr)
            CFRelease (info.cfNameString);

        if (! meter || isJunk (name) || info.unit == kAudioUnitParameterUnit_Boolean || info.maxValue <= info.minValue)
            continue;

        PluginMeter m;
        m.name = name;
        m.isAU = true;
        m.auParamId = id;
        m.minValue = info.minValue;
        m.restValue = juce::jlimit (info.minValue, info.maxValue, info.defaultValue);
        m.maxValue = info.maxValue;
        m.decibels = info.unit == kAudioUnitParameterUnit_Decibels;
        m.unit = m.decibels ? "dB" : (info.unit == kAudioUnitParameterUnit_Percent ? "%" : "");
        classify (m);
        result.add (m);
    }

    return result;
}

/** Returns true when the controller was reachable (result is then authoritative). */
static bool vst3Meters (juce::AudioPluginInstance& p, Steinberg::Vst::IComponent* component, juce::Array<PluginMeter>& result)
{
    using namespace Steinberg;

    if (component == nullptr)
        return false;

    Vst::IEditController* controller = nullptr;

    if (component->queryInterface (Vst::IEditController_iid, (void**) &controller) != kResultOk || controller == nullptr)
        return false;

    juce::Array<Vst::ParamID> readOnly;

    for (int32 i = 0; i < controller->getParameterCount(); ++i)
    {
        Vst::ParameterInfo info {};

        if (controller->getParameterInfo (i, info) == kResultOk
            && (info.flags & Vst::ParameterInfo::kIsReadOnly) != 0
            && (info.flags & (Vst::ParameterInfo::kIsProgramChange | Vst::ParameterInfo::kIsBypass)) == 0)
            readOnly.add (info.id);
    }

    controller->release();

    const auto& params = p.getParameters();

    for (int i = 0; i < params.size(); ++i)
        if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (params[i]))
            if (readOnly.contains ((Vst::ParamID) hosted->getParameterID().getLargeIntValue()) && ! isJunk (params[i]->getName (64)))
            {
                PluginMeter m;
                m.name = params[i]->getName (64);
                m.paramIndex = i;
                classify (m);
                result.add (m);
            }

    return true;
}

juce::Array<PluginMeter> findMeters (juce::AudioPluginInstance& p)
{
    if (auto* au = p.getAudioUnitClient(); au != nullptr && au->getAudioUnitHandle() != nullptr)
        return auMeters (au->getAudioUnitHandle());

    juce::Array<PluginMeter> result;

    if (auto* v3 = p.getVST3Client())
        if (vst3Meters (p, v3->getIComponentPtr(), result))
            return result;

    // No flag to go by: read-only parameters show up as non-automatable ones.
    const auto& params = p.getParameters();

    for (int i = 0; i < params.size() && i < 1024; ++i)
    {
        if (params[i]->isAutomatable() || ! looksLikeMeter (*params[i]))
            continue;

        PluginMeter m;
        m.name = params[i]->getName (64);
        m.paramIndex = i;
        classify (m);
        result.add (m);
    }

    return result;
}

static bool readAU (juce::AudioPluginInstance& p, const PluginMeter& m, float& raw)
{
    auto* au = p.getAudioUnitClient();
    AudioUnitParameterValue v = 0;

    if (au == nullptr || au->getAudioUnitHandle() == nullptr
        || AudioUnitGetParameter (au->getAudioUnitHandle(), m.auParamId, kAudioUnitScope_Global, 0, &v) != noErr)
        return false;

    raw = v;
    return true;
}

float readMeter (juce::AudioPluginInstance& p, const PluginMeter& m)
{
    if (m.isAU)
    {
        float raw = 0.0f;

        if (! readAU (p, m, raw))
            return 0.0f;

        if (m.decibels)
        {
            // show the top 60 dB of a level meter; gain reduction as its distance from 0
            const auto lo = juce::jmax (m.minValue, m.maxValue - 60.0f);
            if (m.gainReduction && m.maxValue <= 0.0f)
                return juce::jlimit (0.0f, 1.0f, -raw / juce::jmax (1.0f, -m.minValue));
            return juce::jlimit (0.0f, 1.0f, (raw - lo) / (m.maxValue - lo));
        }

        // other units: how far the meter moved from where it rests (e.g. a needle resting mid-scale)
        return juce::jlimit (0.0f, 1.0f, std::abs (raw - m.restValue) / (m.maxValue - m.minValue));
    }

    const auto& params = p.getParameters();
    return juce::isPositiveAndBelow (m.paramIndex, params.size()) ? juce::jlimit (0.0f, 1.0f, params[m.paramIndex]->getValue()) : 0.0f;
}

juce::String meterText (juce::AudioPluginInstance& p, const PluginMeter& m)
{
    if (m.isAU)
    {
        float raw = 0.0f;
        return readAU (p, m, raw) ? juce::String (raw, std::abs (raw) < 10.0f ? 1 : 0) + (m.unit.isNotEmpty() ? " " + m.unit : juce::String())
                                  : juce::String();
    }

    const auto& params = p.getParameters();
    return juce::isPositiveAndBelow (m.paramIndex, params.size()) ? params[m.paramIndex]->getCurrentValueAsText() : juce::String();
}

} // namespace stagerack
