#pragma once

#include "../Engine/Session.h"

namespace stagerack
{

/**
    Selection, clipboard and editing commands shared by the rack/slot components and the
    keyboard shortcuts.

    Selection is either a set of racks, or one plugin slot.
*/
class EditController final : public juce::ChangeBroadcaster
{
public:
    explicit EditController (Session& s) : session (s) {}

    //==============================================================================
    // Selection
    void clickRack (int rackId, const juce::ModifierKeys&);   // plain / cmd = toggle / shift = range
    void selectSlot (int rackId, int slot);
    void selectAllRacks();
    void clearSelection();

    bool isRackSelected (int rackId) const          { return selectedRacks.contains (rackId); }
    bool isSlotSelected (int rackId, int slot) const { return slotRack == rackId && slotIndex == slot; }
    bool hasSlotSelection() const                    { return slotRack >= 0; }

    /** Racks a command applies to: the selection, or the slot's rack. */
    juce::Array<int> targetRacks() const;

    /** "Alt = all selected": with Alt held on a control of a rack that is part of the selection,
        the change also goes to the other selected racks. Returns those other racks (rack order),
        or nothing (no Alt, or the rack is not selected). */
    juce::Array<Rack*> linkedRacks (const Rack& origin, const juce::ModifierKeys&) const;

    /** Same, for a plugin slot: the slots of the other selected racks holding the same plugin
        (same format/identifier). */
    std::vector<std::pair<Rack*, int>> linkedSlots (const Rack& origin, int slot, const juce::ModifierKeys&) const;

    //==============================================================================
    // Commands
    void copy();
    void paste();
    void duplicate();
    void deleteSelection();
    void resizeTargets (float factor);                         // < 1 shrinks, > 1 enlarges
    void consecutiveInputs (bool stereo);
    void consecutiveOutputs (bool stereo);

    /** Drag & drop results. */
    void dropPlugin (int fromRack, int fromSlot, int toRack, int toSlot, bool copy);
    void dropRack (int rackId, int targetIndex, bool copy);

    std::function<void (int rackId)> onRackSizeChanged;
    std::function<void()> onUserAction;     // e.g. give the main window keyboard focus

    static constexpr float minRackSize = 0.5f, maxRackSize = 2.2f;

private:
    void changed()                                   { if (onUserAction) onUserAction(); sendChangeMessage(); }

    Session& session;
    juce::Array<int> selectedRacks;
    int anchorRack = -1;
    int slotRack = -1, slotIndex = -1;

    enum class ClipType { none, plugin, racks };
    ClipType clipType = ClipType::none;
    juce::ValueTree clipPlugin;
    std::vector<juce::ValueTree> clipRacks;
};

} // namespace stagerack
