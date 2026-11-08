#include "EditController.h"

namespace stagerack
{

void EditController::clickRack (int rackId, const juce::ModifierKeys& mods)
{
    auto& engine = session.getEngine();
    slotRack = slotIndex = -1;

    if (mods.isShiftDown() && anchorRack >= 0)
    {
        auto* a = engine.findRack (anchorRack);
        auto* b = engine.findRack (rackId);

        if (a != nullptr && b != nullptr)
        {
            const auto lo = juce::jmin (engine.indexOf (*a), engine.indexOf (*b));
            const auto hi = juce::jmax (engine.indexOf (*a), engine.indexOf (*b));
            selectedRacks.clearQuick();

            for (int i = lo; i <= hi; ++i)
                selectedRacks.add (engine.getRack (i).getId());
        }
    }
    else if (mods.isCommandDown())
    {
        if (selectedRacks.contains (rackId))
            selectedRacks.removeFirstMatchingValue (rackId);
        else
            selectedRacks.add (rackId);

        anchorRack = rackId;
    }
    else
    {
        selectedRacks = { rackId };
        anchorRack = rackId;
    }

    changed();
}

void EditController::selectSlot (int rackId, int slot)
{
    selectedRacks.clearQuick();
    slotRack = rackId;
    slotIndex = slot;
    anchorRack = rackId;
    changed();
}

void EditController::selectAllRacks()
{
    selectedRacks.clearQuick();
    slotRack = slotIndex = -1;

    for (int i = 0; i < session.getEngine().getNumRacks(); ++i)
        selectedRacks.add (session.getEngine().getRack (i).getId());

    changed();
}

void EditController::clearSelection()
{
    selectedRacks.clearQuick();
    slotRack = slotIndex = -1;
    changed();
}

juce::Array<int> EditController::targetRacks() const
{
    if (! selectedRacks.isEmpty())
        return selectedRacks;

    if (slotRack >= 0)
        return { slotRack };

    return {};
}

juce::Array<Rack*> EditController::linkedRacks (const Rack& origin, const juce::ModifierKeys& mods) const
{
    juce::Array<Rack*> result;

    if (! mods.isAltDown() || ! selectedRacks.contains (origin.getId()))
        return result;

    auto& engine = session.getEngine();

    for (int i = 0; i < engine.getNumRacks(); ++i)
        if (auto& r = engine.getRack (i); &r != &origin && selectedRacks.contains (r.getId()))
            result.add (&r);

    return result;
}

std::vector<std::pair<Rack*, int>> EditController::linkedSlots (const Rack& origin, int slot, const juce::ModifierKeys& mods) const
{
    std::vector<std::pair<Rack*, int>> result;
    auto* p = origin.getPlugin (slot);

    if (p == nullptr)
        return result;

    const auto ident = p->getPluginDescription().createIdentifierString();

    for (auto* r : linkedRacks (origin, mods))
        for (int s = 0; s < Rack::maxSlots; ++s)
            if (auto* other = r->getPlugin (s); other != nullptr && other->getPluginDescription().createIdentifierString() == ident)
                result.push_back ({ r, s });

    return result;
}

//==============================================================================
void EditController::copy()
{
    auto& engine = session.getEngine();

    if (hasSlotSelection())
    {
        if (auto* r = engine.findRack (slotRack); r != nullptr && r->getPlugin (slotIndex) != nullptr)
        {
            clipPlugin = session.pluginToTree (*r, slotIndex);
            clipType = ClipType::plugin;
        }
        return;
    }

    clipRacks.clear();

    for (int i = 0; i < engine.getNumRacks(); ++i)
        if (selectedRacks.contains (engine.getRack (i).getId()))
            clipRacks.push_back (session.rackToTree (engine.getRack (i)));

    if (! clipRacks.empty())
        clipType = ClipType::racks;
}

void EditController::paste()
{
    auto& engine = session.getEngine();

    if (clipType == ClipType::plugin && clipPlugin.isValid())
    {
        // into the selected slot, or the first free slot of each selected rack
        if (hasSlotSelection())
        {
            if (auto* r = engine.findRack (slotRack))
                session.loadPluginFromTree (*r, slotIndex, clipPlugin);
            return;
        }

        for (auto id : selectedRacks)
            if (auto* r = engine.findRack (id))
                for (int s = 0; s < Rack::maxSlots; ++s)
                    if (r->getPlugin (s) == nullptr)
                    {
                        session.loadPluginFromTree (*r, s, clipPlugin);
                        break;
                    }
        return;
    }

    if (clipType == ClipType::racks)
    {
        // after the last selected rack, or at the end
        int insertAt = engine.getNumRacks();

        for (int i = 0; i < engine.getNumRacks(); ++i)
            if (targetRacks().contains (engine.getRack (i).getId()))
                insertAt = i + 1;

        selectedRacks.clearQuick();
        slotRack = slotIndex = -1;

        for (auto& t : clipRacks)
            selectedRacks.add (session.createRackFromTree (t, insertAt++).getId());

        changed();
    }
}

void EditController::duplicate()
{
    auto& engine = session.getEngine();

    if (hasSlotSelection())
    {
        // copy of the plugin into the next free slot of the same rack
        if (auto* r = engine.findRack (slotRack); r != nullptr && r->getPlugin (slotIndex) != nullptr)
            for (int s = slotIndex + 1; s < Rack::maxSlots; ++s)
                if (r->getPlugin (s) == nullptr)
                {
                    session.copyPlugin (*r, slotIndex, *r, s);
                    selectSlot (slotRack, s);
                    break;
                }
        return;
    }

    if (! selectedRacks.isEmpty())
        session.duplicateRacks (selectedRacks);
}

void EditController::deleteSelection()
{
    auto& engine = session.getEngine();

    if (hasSlotSelection())
    {
        if (auto* r = engine.findRack (slotRack); r != nullptr && r->getPlugin (slotIndex) != nullptr)
            session.removePlugin (*r, slotIndex);
        return;
    }

    if (selectedRacks.isEmpty())
        return;

    // Deleting racks on stage is not undoable: ask first.
    const auto ids = selectedRacks;
    const auto text = ids.size() == 1 ? juce::String ("Delete the selected rack?")
                                      : "Delete " + juce::String (ids.size()) + " racks?";

    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "StageRack", text, "Delete", "Cancel", nullptr,
        juce::ModalCallbackFunction::create ([this, ids] (int result)
        {
            if (result == 0)
                return;

            for (auto id : ids)
                if (auto* r = session.getEngine().findRack (id))
                    session.removeRack (*r);

            clearSelection();
        }));
}

void EditController::resizeTargets (float factor)
{
    auto ids = targetRacks();

    if (ids.isEmpty())
        for (int i = 0; i < session.getEngine().getNumRacks(); ++i)
            ids.add (session.getEngine().getRack (i).getId());

    for (auto id : ids)
    {
        if (auto* r = session.getEngine().findRack (id))
        {
            r->uiSize = juce::jlimit (minRackSize, maxRackSize, r->uiSize * factor);

            if (onRackSizeChanged)
                onRackSizeChanged (id);
        }
    }

    session.markDirty();
}

void EditController::consecutiveInputs (bool stereo)
{
    session.setConsecutiveInputs (targetRacks(), stereo);
}

void EditController::consecutiveOutputs (bool stereo)
{
    session.setConsecutiveOutputs (targetRacks(), stereo);
}

void EditController::dropPlugin (int fromRack, int fromSlot, int toRack, int toSlot, bool copy)
{
    auto& engine = session.getEngine();
    auto* a = engine.findRack (fromRack);
    auto* b = engine.findRack (toRack);

    if (a == nullptr || b == nullptr || (a == b && fromSlot == toSlot))
        return;

    if (copy)
        session.copyPlugin (*a, fromSlot, *b, toSlot);
    else
        session.movePlugin (*a, fromSlot, *b, toSlot);

    selectSlot (toRack, toSlot);
}

void EditController::dropRack (int rackId, int targetIndex, bool copy)
{
    auto& engine = session.getEngine();
    auto* r = engine.findRack (rackId);

    if (r == nullptr)
        return;

    if (copy)
    {
        auto t = session.rackToTree (*r);
        t.setProperty ("name", r->name + " copy", nullptr);
        auto& created = session.createRackFromTree (t, targetIndex);
        selectedRacks = { created.getId() };
        changed();
        return;
    }

    const auto from = engine.indexOf (*r);
    session.moveRackTo (rackId, targetIndex > from ? targetIndex - 1 : targetIndex);
}

} // namespace stagerack
