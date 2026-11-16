#include "EditController.h"
#include "PluginMeters.h"

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

juce::Array<Rack*> EditController::linkedRacks (const Rack& origin, const juce::ModifierKeys& mods, Link kind) const
{
    juce::Array<Rack*> result;
    auto& engine = session.getEngine();

    const bool bySelection = selectedRacks.contains (origin.getId())
                          && (mods.isAltDown() || (kind == Link::plugin && mods.isShiftDown()));
    const int group = kind != Link::routing && session.isEditGroupActive (origin.editGroup) ? origin.editGroup : 0;

    if (! bySelection && group == 0)
        return result;

    for (int i = 0; i < engine.getNumRacks(); ++i)
        if (auto& r = engine.getRack (i); &r != &origin
              && ((bySelection && selectedRacks.contains (r.getId())) || (group > 0 && r.editGroup == group)))
            result.add (&r);

    return result;
}

bool EditController::linkedBySelection (const Rack& origin, const Rack& other, const juce::ModifierKeys& mods) const
{
    return mods.isAltDown() && selectedRacks.contains (origin.getId()) && selectedRacks.contains (other.getId());
}

std::vector<std::pair<Rack*, int>> EditController::linkedSlots (const Rack& origin, int slot, const juce::ModifierKeys& mods, Link kind) const
{
    std::vector<std::pair<Rack*, int>> result;
    auto* p = origin.getPlugin (slot);

    if (p == nullptr)
        return result;

    const auto ident = p->getPluginDescription().createIdentifierString();
    auto holds = [&ident] (Rack& r, int s)
    {
        auto* other = r.getPlugin (s);
        return other != nullptr && other->getPluginDescription().createIdentifierString() == ident;
    };

    for (auto* r : linkedRacks (origin, mods, kind))
    {
        if (holds (*r, slot))
        {
            result.push_back ({ r, slot });
            continue;
        }

        for (int s = 0; s < Rack::maxSlots; ++s)
            if (holds (*r, s))
            {
                result.push_back ({ r, s });
                break;
            }
    }

    return result;
}

//==============================================================================
void EditController::syncGroupParameters()
{
    auto& engine = session.getEngine();

    // a new session frees plugins (their addresses can come back): start over
    if (snapshotLoadGeneration != session.getLoadGeneration())
    {
        paramSnapshots.clear();
        snapshotLoadGeneration = session.getLoadGeneration();
    }

    // a scene recall sets every instance on its own: never mirror those changes
    const bool recalled = snapshotRecallCount != session.getRecallCount();
    snapshotRecallCount = session.getRecallCount();

    std::map<const juce::AudioPluginInstance*, ParamSnapshot> seen;

    for (int group = 1; group <= Rack::maxEditGroups; ++group)
    {
        if (! session.isEditGroupActive (group))
            continue;

        // linked sets: the k-th instance of the same plugin in each rack of the group
        std::map<std::pair<juce::String, int>, juce::Array<juce::AudioPluginInstance*>> sets;

        for (int i = 0; i < engine.getNumRacks(); ++i)
        {
            auto& r = engine.getRack (i);

            if (r.editGroup != group)
                continue;

            std::map<juce::String, int> count;

            for (int s = 0; s < Rack::maxSlots; ++s)
                if (auto* p = r.getPlugin (s))
                {
                    const auto ident = p->getPluginDescription().createIdentifierString();
                    sets[{ ident, count[ident]++ }].add (p);
                }
        }

        for (auto& [key, instances] : sets)
        {
            // snapshots (new instances: just remember them)
            std::vector<ParamSnapshot*> snaps;
            bool allKnown = true;

            for (auto* p : instances)
            {
                auto it = paramSnapshots.find (p);

                if (it == paramSnapshots.end() || it->second.values.size() != p->getParameters().size())
                {
                    ParamSnapshot snap;
                    const auto& params = p->getParameters();

                    for (int n = 0; n < params.size(); ++n)
                    {
                        snap.values.add (params[n]->getValue());
                        const auto cat = params[n]->getCategory();
                        if (! params[n]->isAutomatable() || (cat != juce::AudioProcessorParameter::genericParameter
                                                             && cat != juce::AudioProcessorParameter::inputGain
                                                             && cat != juce::AudioProcessorParameter::outputGain))
                            snap.skip.setBit (n);
                    }

                    for (auto& m : findMeters (*p))
                        if (m.paramIndex >= 0)
                            snap.skip.setBit (m.paramIndex);

                    it = paramSnapshots.insert_or_assign (p, std::move (snap)).first;
                    allKnown = false;
                }

                snaps.push_back (&it->second);
            }

            if (instances.size() > 1 && allKnown && ! recalled)
            {
                const int numParams = instances.getFirst()->getParameters().size();

                for (int n = 0; n < numParams; ++n)
                {
                    int changed = -1, numChanged = 0;

                    for (int k = 0; k < instances.size(); ++k)
                        if (n < instances[k]->getParameters().size() && ! snaps[(size_t) k]->skip[n]
                              && std::abs (instances[k]->getParameters()[n]->getValue() - snaps[(size_t) k]->values[n]) > 1.0e-6f)
                        {
                            changed = k;
                            ++numChanged;
                        }

                    if (numChanged != 1)
                        continue;

                    const auto v = instances[changed]->getParameters()[n]->getValue();

                    for (int k = 0; k < instances.size(); ++k)
                        if (k != changed && n < instances[k]->getParameters().size())
                            if (auto* prm = instances[k]->getParameters()[n]; std::abs (prm->getValue() - v) > 1.0e-6f)
                            {
                                prm->beginChangeGesture();
                                prm->setValueNotifyingHost (v);
                                prm->endChangeGesture();
                            }
                }
            }

            // look again next time from what they hold now
            for (size_t k = 0; k < snaps.size(); ++k)
            {
                const auto& params = instances[(int) k]->getParameters();
                for (int n = 0; n < params.size() && n < snaps[k]->values.size(); ++n)
                    snaps[k]->values.set (n, params[n]->getValue());
            }

            for (auto* p : instances)
                seen[p] = std::move (paramSnapshots[p]);
        }
    }

    paramSnapshots = std::move (seen);   // instances that left a group are forgotten
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
        {
            // the same plugin goes from the other racks of an active edit group too
            auto where = linkedSlots (*r, slotIndex, {}, Link::plugin);
            where.insert (where.begin(), { r, slotIndex });
            session.removePlugins (where);
        }
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
