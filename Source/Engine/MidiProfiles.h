#pragma once

#include "MidiMap.h"

namespace stagerack
{

/**
    Known controllers: names for their controls ("Fader 3" instead of "CC 27") and a ready-made
    layout (Quick setup). Anything else works through MIDI learn with generic names; LED feedback
    echoes the same note / CC back, which is what most controllers expect.
*/
namespace midiprofiles
{
    enum Profile { generic = 0, akaiMidiMix, korgNanoKontrol2 };

    inline Profile detect (const juce::String& deviceName)
    {
        const auto n = deviceName.toLowerCase().removeCharacters (" -_");
        if (n.contains ("midimix"))        return akaiMidiMix;
        if (n.contains ("nanokontrol2"))   return korgNanoKontrol2;
        return generic;
    }

    inline juce::String profileName (Profile p)
    {
        switch (p)
        {
            case akaiMidiMix:      return "Akai MIDImix";
            case korgNanoKontrol2: return "Korg nanoKONTROL2";
            case generic:          break;
        }
        return "Generic controller";
    }

    // Akai MIDImix (factory mapping, channel 1). Strip c = 0..7: knobs A/B/C and fader are CCs,
    // Mute / Solo (Mute while SOLO is held) / Rec Arm are notes; their LEDs light with the same
    // note (velocity 127 = on, 0 = off). Bank Left/Right have LEDs too.
    namespace midimix
    {
        inline int knobCC (int strip, int row) noexcept { return (strip < 4 ? 16 + strip * 4 : 46 + (strip - 4) * 4) + row; }
        inline int faderCC (int strip) noexcept         { return knobCC (strip, 3); }
        constexpr int masterCC = 62;
        inline int muteNote (int strip) noexcept        { return 1 + strip * 3; }
        inline int soloNote (int strip) noexcept        { return 2 + strip * 3; }
        inline int recNote (int strip) noexcept         { return 3 + strip * 3; }
        constexpr int bankLeftNote = 25, bankRightNote = 26, soloButtonNote = 27;
    }

    // Korg nanoKONTROL2 (factory CC mode, channel 1). LEDs follow the host only with
    // "LED Mode: External" (Korg Kontrol Editor).
    namespace nano
    {
        inline int faderCC (int s) noexcept { return s; }
        inline int knobCC (int s) noexcept  { return 16 + s; }
        inline int soloCC (int s) noexcept  { return 32 + s; }
        inline int muteCC (int s) noexcept  { return 48 + s; }
        inline int recCC (int s) noexcept   { return 64 + s; }
        constexpr int trackPrev = 58, trackNext = 59, cycle = 46, markerSet = 60, markerPrev = 61, markerNext = 62,
                      rew = 43, ff = 44, stop = 42, play = 41, rec = 45;
    }

    /** "Fader 3", "Mute 1", "Knob 2B"... or empty if this controller / control is not known. */
    inline juce::String controlName (Profile p, const MidiSource& s)
    {
        if (s.channel != 1)
            return {};

        if (p == akaiMidiMix)
        {
            using namespace midimix;
            if (s.type == MidiSource::cc)
            {
                if (s.number == masterCC) return "Master fader";
                for (int c = 0; c < 8; ++c)
                {
                    if (s.number == faderCC (c)) return "Fader " + juce::String (c + 1);
                    for (int r = 0; r < 3; ++r)
                        if (s.number == knobCC (c, r)) return "Knob " + juce::String (c + 1) + juce::String::charToString ((juce::juce_wchar) ('A' + r));
                }
            }
            else if (s.type == MidiSource::note)
            {
                if (s.number == bankLeftNote)   return "Bank Left";
                if (s.number == bankRightNote)  return "Bank Right";
                if (s.number == soloButtonNote) return "SOLO";
                if (s.number >= 1 && s.number <= 24)
                {
                    const auto c = (s.number - 1) / 3, k = (s.number - 1) % 3;
                    return juce::String (k == 0 ? "Mute " : k == 1 ? "Solo " : "Rec Arm ") + juce::String (c + 1);
                }
            }
        }
        else if (p == korgNanoKontrol2 && s.type == MidiSource::cc)
        {
            using namespace nano;
            const auto n = s.number;
            if (n >= 0 && n < 8)   return "Fader " + juce::String (n + 1);
            if (n >= 16 && n < 24) return "Knob " + juce::String (n - 15);
            if (n >= 32 && n < 40) return "S " + juce::String (n - 31);
            if (n >= 48 && n < 56) return "M " + juce::String (n - 47);
            if (n >= 64 && n < 72) return "R " + juce::String (n - 63);
            switch (n)
            {
                case trackPrev: return "Track <";   case trackNext: return "Track >";
                case cycle: return "Cycle";         case markerSet: return "Marker Set";
                case markerPrev: return "Marker <"; case markerNext: return "Marker >";
                case rew: return "Rewind";          case ff: return "Forward";
                case stop: return "Stop";           case play: return "Play";   case rec: return "Rec";
                default: break;
            }
        }
        return {};
    }

    /** Ready-made layout for a known controller (a "mixer" on the bank of 8 racks). */
    inline std::vector<MidiMapping> quickSetup (Profile p, const juce::String& deviceId, const juce::String& deviceName)
    {
        std::vector<MidiMapping> v;

        auto add = [&] (int type, int number, MidiTarget t, int mode = MidiMapping::autoMode)
        {
            MidiMapping m;
            m.device = deviceId; m.deviceName = deviceName;
            m.source.type = type; m.source.channel = 1; m.source.number = number;
            m.target = t; m.mode = mode;
            v.push_back (m);
            return &v.back();
        };
        using T = MidiTarget;

        if (p == akaiMidiMix)
        {
            using namespace midimix;
            for (int c = 0; c < 8; ++c)
            {
                add (MidiSource::cc, faderCC (c), T::forRack (T::rackGain, T::bankRack, c));
                add (MidiSource::cc, knobCC (c, 0), T::forRack (T::rackInput, T::bankRack, c));
                add (MidiSource::cc, knobCC (c, 1), T::forRack (T::rackSend, T::bankRack, c, 1));
                add (MidiSource::cc, knobCC (c, 2), T::forRack (T::rackSend, T::bankRack, c, 2));
                add (MidiSource::note, muteNote (c), T::forRack (T::rackMute, T::bankRack, c))->layer = MidiMapping::noShift;
                // Solo: the Mute buttons while SOLO is held; their LEDs then show the solos
                auto* solo = add (MidiSource::note, soloNote (c), T::forRack (T::rackSolo, T::bankRack, c));
                solo->layer = MidiMapping::withShift;
                solo->feedbackNumber = muteNote (c);
                add (MidiSource::note, recNote (c), T::forRack (T::rackBypass, T::bankRack, c));
            }
            add (MidiSource::note, bankLeftNote, T::global (T::bankPrev));
            add (MidiSource::note, bankRightNote, T::global (T::bankNext));
            add (MidiSource::note, soloButtonNote, T::global (T::shift), MidiMapping::momentary);
        }
        else if (p == korgNanoKontrol2)
        {
            using namespace nano;
            for (int c = 0; c < 8; ++c)
            {
                add (MidiSource::cc, faderCC (c), T::forRack (T::rackGain, T::bankRack, c));
                add (MidiSource::cc, knobCC (c), T::forRack (T::rackInput, T::bankRack, c));
                add (MidiSource::cc, soloCC (c), T::forRack (T::rackSolo, T::bankRack, c), MidiMapping::toggle);
                add (MidiSource::cc, muteCC (c), T::forRack (T::rackMute, T::bankRack, c), MidiMapping::toggle);
                add (MidiSource::cc, recCC (c), T::forRack (T::rackBypass, T::bankRack, c), MidiMapping::toggle);
            }
            add (MidiSource::cc, trackPrev, T::global (T::bankPrev), MidiMapping::trigger);
            add (MidiSource::cc, trackNext, T::global (T::bankNext), MidiMapping::trigger);
            add (MidiSource::cc, markerPrev, T::global (T::scenePrev), MidiMapping::trigger);
            add (MidiSource::cc, markerNext, T::global (T::sceneNext), MidiMapping::trigger);
        }
        return v;
    }
}

} // namespace stagerack
