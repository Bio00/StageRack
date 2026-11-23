#pragma once

#include <JuceHeader.h>
#include "../Engine/Features.h"

namespace stagerack
{

/** Commands of the macOS menu bar (and their default shortcuts). */
namespace cmd
{
    enum : juce::CommandID
    {
        newSession = 0x5201, openSession, saveSession, saveSessionAs,
        undo, redo, copy, paste, duplicate, deleteSelection, selectAll, deselect,
        consecutiveInMono, consecutiveInStereo, consecutiveOutMono, consecutiveOutStereo,
        addRack, addRacks, biggerRacks, smallerRacks,
        nextScene, prevScene, storeScene, addScene,
        masterMute, automixOnOff, audioSettings, pluginManager,
        toggleLayout, toggleScenes, automixerWindow, lightTheme, tintRacks, editGroupsOn,
        tempoWindow, tapMaster, editGroupsWindow, addTempo,
        routingOnOff, addBus, groupFolder,
        midiMap, midiWindow, sendKnobs,
        lastCommand = sendKnobs
    };

    /** Editing commands act on the racks only while the main window has the keyboard: the same
        shortcut typed in a plugin window must not undo or delete something in a rack. */
    inline bool isRackEdit (juce::CommandID id)
    {
        return (id >= undo && id <= consecutiveOutStereo) || id == biggerRacks || id == smallerRacks;
    }

    /** The macro function a command belongs to (SETTINGS > Features), -1 = always there. */
    inline int featureOf (juce::CommandID id)
    {
        switch (id)
        {
            case nextScene: case prevScene: case storeScene: case addScene: case toggleScenes: return features::snapshots;
            case automixOnOff: case automixerWindow:                                           return features::automix;
            case tempoWindow: case tapMaster: case addTempo:                                    return features::tempo;
            case editGroupsOn: case editGroupsWindow:                                           return features::editGroups;
            case routingOnOff: case addBus: case groupFolder: case sendKnobs:                   return features::routing;
            case midiMap: case midiWindow:                                                      return features::midi;
            default:                                                                            return -1;
        }
    }
}

/** True if the event being handled right now is a key press (a menu shortcut, not a click). */
bool currentEventIsKeyDown();

/** Key presses that reach a window of ours before it handles them (an NSEvent local monitor).
    The handler gets the window's content view (a JUCE peer's native handle), the key as a JUCE
    key code (lower-case letter, ' ' for space), whether it is an auto-repeat, whether Cmd/Ctrl/Alt
    are down and whether a native text field has the keyboard. Returning true swallows the key.
    Used for the tap tempo keys while a plugin editor has the keyboard (user request 2026-11-23). */
void installKeyMonitor (std::function<bool (void* view, int keyCode, bool repeat, bool modifiers, bool typing)>);

/** Call at the top of main(), before JUCE/AppKit start, in child processes: no Dock icon. */
void markAsBackgroundProcess();

/** Sandbox children: no Dock icon, but they may show a window (the plugin's editor). */
void markAsAccessoryProcess();

/** Menu item that remembers whether it was triggered from the keyboard. */
struct KeyOriginCallback final : juce::PopupMenu::CustomCallback
{
    static inline bool lastWasKey = false;
    bool menuItemTriggered() override { lastWasKey = currentEventIsKeyDown(); return true; }
};

} // namespace stagerack
