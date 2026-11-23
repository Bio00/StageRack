#pragma once

#include <JuceHeader.h>
#include <atomic>

/**
    Macro functions the user can switch off in SETTINGS > Features, for a simpler StageRack
    (user request 2026-11-22). A function that is off disappears from the top bar, the menus and
    the racks, AND stops acting: an automixer or edit groups the user can't see must never change
    the sound or the controls. What the session stores is kept as it is (switch it back on: all there).

    App settings (not per session): "feature_<key>" = bool, default on.
*/
namespace stagerack::features
{
    enum Feature { snapshots, automix, tempo, midi, editGroups, routing, numFeatures };

    inline std::atomic<bool> enabled[numFeatures] { true, true, true, true, true, true };

    inline bool has (Feature f) noexcept   { return enabled[f].load (std::memory_order_relaxed); }

    inline const char* key (Feature f)
    {
        static const char* k[numFeatures] = { "snapshots", "automix", "tempo", "midi", "editGroups", "routing" };
        return k[f];
    }

    inline juce::String name (Feature f)
    {
        static const char* n[numFeatures] = { "Snapshots", "Automixer", "Tap tempo", "MIDI controllers", "Edit groups", "Buses and sends" };
        return n[f];
    }

    inline juce::String description (Feature f)
    {
        static const char* d[numFeatures] = {
            "Snapshot list, recall from the top bar, arrows / space, MIDI Program Change and the next / previous CCs.",
            "Gain sharing between microphones (AUTOMIX, AM METERS, rack menu > Automix). Off = every rack at unity gain.",
            "Tap tempos in the top bar, the Tempo window, tap keys, tempo out (MIDI / OSC). Plugins still get the master tempo.",
            "MIDI MAP and the MIDI window: controllers, mappings, LEDs. Off = controllers are ignored (Program Change for snapshots still works).",
            "Racks that follow each other (GROUPS, rack menu > Edit group). Off = every rack on its own; Alt still edits the selected racks.",
            "Buses, sends, folders and outputs to a bus. Off = every rack goes straight to the device outputs, as without buses."
        };
        return d[f];
    }

    inline void load (juce::PropertiesFile& settings)
    {
        for (int i = 0; i < numFeatures; ++i)
            enabled[i] = settings.getBoolValue (juce::String ("feature_") + key ((Feature) i), true);
    }

    inline void save (juce::PropertiesFile& settings)
    {
        for (int i = 0; i < numFeatures; ++i)
            settings.setValue (juce::String ("feature_") + key ((Feature) i), has ((Feature) i));
    }
}
