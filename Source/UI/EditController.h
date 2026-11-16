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

    /** What an edit touches, for linking:
        mix     = gain, M/S/B, plugin parameters/view: Alt on a selected rack, or its active edit group;
        routing = IN/OUT: Alt on a selected rack only (a group never re-routes);
        plugin  = insert/remove/on-off: Alt or Shift on a selected rack, or its active edit group. */
    enum class Link { mix, routing, plugin };

    /** The other racks an edit on `origin` also goes to (rack order), or nothing. */
    juce::Array<Rack*> linkedRacks (const Rack& origin, const juce::ModifierKeys&, Link = Link::mix) const;

    /** True when `other` is linked to `origin` through the selection (Alt), false when only through
        an edit group: group members follow gain changes relatively (they keep their offsets). */
    bool linkedBySelection (const Rack& origin, const Rack& other, const juce::ModifierKeys&) const;

    /** Same, for a plugin slot: in each linked rack, the slot holding the same plugin
        (same format/identifier), preferring the same slot number. One per rack. */
    std::vector<std::pair<Rack*, int>> linkedSlots (const Rack& origin, int slot, const juce::ModifierKeys&,
                                                    Link = Link::mix) const;

    /** Edit groups and plugin editors: a parameter moved in one plugin's own window (or by MIDI
        learn, or anything else) is copied to the same plugin in the other racks of its active
        edit group. Polls the grouped plugins (~30x/s from the main timer, message thread): when exactly
        one instance of a linked set changed a parameter since the last look, the others follow.
        Several changing together (scene recall, Alt edits) are left alone; meters are skipped. */
    void syncGroupParameters();

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

    struct ParamSnapshot
    {
        juce::Array<float> values;
        juce::BigInteger skip;        // meters / read-only parameters
    };
    std::map<const juce::AudioPluginInstance*, ParamSnapshot> paramSnapshots;
    int snapshotLoadGeneration = -1, snapshotRecallCount = -1;

    enum class ClipType { none, plugin, racks };
    ClipType clipType = ClipType::none;
    juce::ValueTree clipPlugin;
    std::vector<juce::ValueTree> clipRacks;
};

} // namespace stagerack
