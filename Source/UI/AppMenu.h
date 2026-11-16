#pragma once

#include <JuceHeader.h>

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
        toggleLayout, toggleScenes, automixerWindow, lightTheme
    };

    /** Editing commands act on the racks only while the main window has the keyboard: the same
        shortcut typed in a plugin window must not undo or delete something in a rack. */
    inline bool isRackEdit (juce::CommandID id)
    {
        return (id >= undo && id <= consecutiveOutStereo) || id == biggerRacks || id == smallerRacks;
    }
}

/** True if the event being handled right now is a key press (a menu shortcut, not a click). */
bool currentEventIsKeyDown();

/** Menu item that remembers whether it was triggered from the keyboard. */
struct KeyOriginCallback final : juce::PopupMenu::CustomCallback
{
    static inline bool lastWasKey = false;
    bool menuItemTriggered() override { lastWasKey = currentEventIsKeyDown(); return true; }
};

} // namespace stagerack
