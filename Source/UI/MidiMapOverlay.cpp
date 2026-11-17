#include "MidiMapOverlay.h"
#include "Theme.h"

namespace stagerack
{

MidiMapOverlay::MidiMapOverlay (juce::Component& r, Session& s, MidiControl& m) : root (r), session (s), midi (m)
{
    setWantsKeyboardFocus (true);
    setAlwaysOnTop (false);

    hint.setFont (theme::font (12.5f, true));
    hint.setColour (juce::Label::textColourId, juce::Colours::white);
    hint.setJustificationType (juce::Justification::centredLeft);
    hint.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (hint);

    auto scope = [this] (juce::TextButton& b, int mode, const juce::String& tip)
    {
        b.setClickingTogglesState (false);
        b.setTooltip (tip);
        b.onClick = [this, mode] { midi.learnRackMode = mode; updateScopeButtons(); repaint(); };
        addAndMakeVisible (b);
    };
    scope (scopeRack, MidiTarget::fixedRack, "A rack's control is mapped to that rack, wherever it is");
    scope (scopeBank, MidiTarget::bankRack, "A rack's control is mapped to its strip of the controller's bank: "
                                            "the same knob drives the n-th rack of whatever bank is selected");
    scope (scopeSelected, MidiTarget::selectedRack, "A rack's control is mapped to the selected rack (one set of knobs for any rack you click)");
    scopeRack.setConnectedEdges (juce::Button::ConnectedOnRight);
    scopeBank.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    scopeSelected.setConnectedEdges (juce::Button::ConnectedOnLeft);

    windowButton.onClick = [this] { if (onOpenWindow) onOpenWindow(); };
    doneButton.onClick = [this] { if (onDone) onDone(); };
    addAndMakeVisible (windowButton);
    addAndMakeVisible (doneButton);
    updateScopeButtons();
}

void MidiMapOverlay::setActive (bool on)
{
    snapshots.clear();
    watchTicks = 0;

    if (on)
    {
        collect();
        startTimerHz (15);
        if (isShowing()) grabKeyboardFocus();
    }
    else
    {
        stopTimer();
        shown.clear();
    }
    updateScopeButtons();
    updateHint();
    repaint();
}

void MidiMapOverlay::updateScopeButtons()
{
    auto style = [this] (juce::TextButton& b, int mode)
    {
        const bool on = midi.learnRackMode == mode;
        b.setColour (juce::TextButton::buttonColourId, on ? theme::midiBlue : theme::slot);
        b.setColour (juce::TextButton::textColourOffId, on ? juce::Colours::white : theme::textDim);
    };
    style (scopeRack, MidiTarget::fixedRack);
    style (scopeBank, MidiTarget::bankRack);
    style (scopeSelected, MidiTarget::selectedRack);
}

//==============================================================================
void MidiMapOverlay::collectFrom (juce::Component& c, std::vector<MidiTargetHit>& out)
{
    for (auto* child : c.getChildren())
    {
        if (child == this || ! child->isVisible() || child->getWidth() <= 0 || child->getHeight() <= 0)
            continue;

        if (auto* p = dynamic_cast<MidiTargetProvider*> (child))
        {
            std::vector<MidiTargetHit> hits;
            p->collectMidiTargets (hits);

            for (auto& h : hits)
            {
                auto area = getLocalArea (child, h.bounds);

                // clip to what is visible (racks and slots scroll inside viewports)
                for (auto* a = child; a != nullptr && a != &root; a = a->getParentComponent())
                    area = area.getIntersection (getLocalArea (a, a->getLocalBounds()));

                if (area.getWidth() > 3 && area.getHeight() > 3)
                    out.push_back ({ area, h.target });
            }
        }

        collectFrom (*child, out);
    }
}

void MidiMapOverlay::collect()
{
    std::vector<MidiTargetHit> hits;
    collectFrom (root, hits);

    shown.clear();
    for (auto& h : hits)
        shown.push_back ({ h.bounds, h.target });
}

void MidiMapOverlay::timerCallback()
{
    // the band moves to the top while the mouse works near the bottom (faders of vertical racks)
    const auto mouse = getLocalPoint (nullptr, juce::Desktop::getMousePosition());
    const bool top = getLocalBounds().contains (mouse) && mouse.y > getHeight() - 140;
    if (top != bandAtTop)
    {
        bandAtTop = top;
        resized();
    }

    collect();
    watchPluginParameters();

    // keep the keyboard (Esc / Delete) unless another window has it
    if (! hasKeyboardFocus (true) && getTopLevelComponent()->hasKeyboardFocus (true))
        grabKeyboardFocus();

    updateHint();
    repaint();
}

/** A parameter moved in a plugin's own window becomes the control to map. Parameters that move
    by themselves (meters, LFOs) are seen moving during the first second and ignored. */
void MidiMapOverlay::watchPluginParameters()
{
    ++watchTicks;
    auto& engine = session.getEngine();
    const bool midiMoved = midi.getEventCount() != midiEventsAtLastTick;
    midiEventsAtLastTick = midi.getEventCount();

    struct Change { Rack* rack; int slot, param; };
    std::vector<Change> changes;

    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        auto& r = engine.getRack (i);

        for (int s = 0; s < Rack::maxSlots; ++s)
        {
            auto* p = r.getPlugin (s);
            if (p == nullptr)
                continue;

            auto& params = p->getParameters();
            auto& snap = snapshots[p];
            const bool fresh = snap.values.size() != (size_t) params.size();

            if (fresh)
            {
                snap.values.assign ((size_t) params.size(), 0.0f);
                snap.moving.assign ((size_t) params.size(), false);
            }

            for (int k = 0; k < params.size(); ++k)
            {
                const auto v = params[k]->getValue();

                if (! fresh && std::abs (v - snap.values[(size_t) k]) > 1.0e-5f)
                {
                    if (watchTicks <= 15)
                        snap.moving[(size_t) k] = true;   // moves by itself
                    else if (! snap.moving[(size_t) k] && params[k]->isAutomatable())
                        changes.push_back ({ &r, s, k });
                }
                snap.values[(size_t) k] = v;
            }
        }
    }

    // one or two parameters moved (not a scene recall, not MIDI): that's the user's hand
    if (! changes.empty() && changes.size() <= 2 && ! midiMoved)
    {
        auto& c = changes.front();
        auto* prm = c.rack->getPlugin (c.slot)->getParameters()[c.param];
        auto t = MidiTarget::forRack (MidiTarget::pluginParam, MidiTarget::fixedRack, c.rack->getId(), c.slot, c.param);
        t.paramName = prm->getName (64);

        const auto learn = asLearnTarget (t);
        if (! (learn == midi.getLearnTarget()))
            midi.setLearnTarget (learn);
    }
}

//==============================================================================
MidiTarget MidiMapOverlay::asLearnTarget (const MidiTarget& fixed) const
{
    if (! fixed.isRackTarget() || midi.learnRackMode == MidiTarget::fixedRack)
        return fixed;

    auto t = fixed;
    if (midi.learnRackMode == MidiTarget::selectedRack)
    {
        t.rackMode = MidiTarget::selectedRack;
        t.rack = 0;
        return t;
    }

    if (auto* r = session.getEngine().findRack (fixed.rack))
        if (const auto pos = midi.bankPosition (*r); pos >= 0)
        {
            t.rackMode = MidiTarget::bankRack;
            t.rack = pos;
            return t;
        }

    return fixed;   // not in the controller's bank: this rack
}

bool MidiMapOverlay::isLearnTarget (const MidiTarget& fixed) const
{
    const auto& l = midi.getLearnTarget();
    if (! l.isValid() || l.kind != fixed.kind || l.index != fixed.index || l.param != fixed.param)
        return false;
    if (! fixed.isRackTarget())
        return true;
    auto* r = midi.resolveRack (l);
    return r != nullptr && r->getId() == fixed.rack;
}

std::vector<int> MidiMapOverlay::mappingsShownAt (const MidiTarget& fixed) const
{
    std::vector<int> ids;
    for (auto& m : midi.getMappings())
    {
        const auto& t = m.target;
        if (t.kind != fixed.kind || t.index != fixed.index || (t.kind == MidiTarget::pluginParam && t.param != fixed.param))
            continue;
        if (fixed.isRackTarget())
        {
            auto* r = midi.resolveRack (t);
            if (r == nullptr || r->getId() != fixed.rack)
                continue;
        }
        ids.push_back (m.id);
    }
    return ids;
}

juce::String MidiMapOverlay::labelFor (const MidiTarget& fixed) const
{
    juce::StringArray names;
    for (auto id : mappingsShownAt (fixed))
        if (auto* m = midi.findMapping (id))
        {
            auto n = midi.describeSource (*m);
            if (m->layer == MidiMapping::withShift) n = "Shift+" + n;
            if (m->target.rackMode == MidiTarget::bankRack && fixed.isRackTarget()) n = juce::String::fromUTF8 ("\xe2\x96\xa4 ") + n;   // bank strip
            if (m->target.rackMode == MidiTarget::selectedRack && fixed.isRackTarget()) n = "Sel " + n;
            names.addIfNotAlreadyThere (n);
        }
    return names.joinIntoString (", ");
}

/** Short form of a control name for small targets: "Shift+Mute 1" -> "^M1", "Knob 2B" -> "K2B", "CC 19" -> "19". */
static juce::String compact (const juce::String& label)
{
    juce::String out;
    for (auto& part : juce::StringArray::fromTokens (label.upToFirstOccurrenceOf (",", false, false), " +", {}))
    {
        if (part == "Shift")             out << "^";
        else if (part == "CC")           continue;
        else if (part.containsOnly ("0123456789ABCDEFGH") && part.isNotEmpty()) out << part;
        else if (part.isNotEmpty() && juce::CharacterFunctions::isLetter (part[0])) out << part.substring (0, 1);
        else                             out << part.substring (0, 1);
    }
    return label.contains (",") ? out + "+" : out;
}

//==============================================================================
void MidiMapOverlay::paint (juce::Graphics& g)
{
    const auto blue = theme::midiBlue;

    for (auto& s : shown)
    {
        const auto label = labelFor (s.target);
        const bool selected = isLearnTarget (s.target);
        auto a = s.area.toFloat().reduced (0.5f);

        g.setColour (selected ? theme::amber.withAlpha (0.28f) : blue.withAlpha (label.isNotEmpty() ? 0.42f : 0.2f));
        g.fillRoundedRectangle (a, 3.0f);
        g.setColour (selected ? theme::amber : blue.withAlpha (0.85f));
        g.drawRoundedRectangle (a, 3.0f, selected ? 2.0f : 1.0f);

        if (label.isNotEmpty())
        {
            g.setFont (theme::font (9.0f, true));
            auto text = label;
            if (juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 8 > (int) a.getWidth())
                text = compact (label);   // "Mute 1" -> "M1" on small buttons
            const auto w = juce::jmin ((int) a.getWidth(), juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 8);
            auto tag = juce::Rectangle<float> (a.getRight() - (float) w, a.getY(), (float) w, 12.0f);
            g.setColour (selected ? theme::amber : blue);
            g.fillRoundedRectangle (tag, 2.5f);
            g.setColour (selected ? juce::Colours::black : juce::Colours::white);
            g.drawFittedText (text, tag.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.6f);
        }
    }

    // instruction band
    g.setColour (juce::Colours::black.withAlpha (0.82f));
    g.fillRoundedRectangle (bandArea.toFloat(), 8.0f);
    g.setColour (blue);
    g.drawRoundedRectangle (bandArea.toFloat().reduced (0.5f), 8.0f, 1.5f);

}

void MidiMapOverlay::updateHint()
{
    // hint text follows the state
    juce::String text;
    const auto& l = midi.getLearnTarget();
    const auto now = juce::Time::getMillisecondCounter();

    if (now < noticeUntil)
        text = notice;
    else if (l.isValid())
    {
        const auto existing = midi.labelFor (l);
        text = juce::String::fromUTF8 ("\xe2\x96\xb6 ") + midi.describeTarget (l) + ":  move a knob, fader or button"
             + (existing.isNotEmpty() ? "  (now: " + existing + ")" : juce::String());
    }
    else
        text = "MIDI MAP  -  click a control (or move a parameter in a plugin window), then move a control on your controller";

    if (hint.getText() != text)
        hint.setText (text, juce::dontSendNotification);
}

void MidiMapOverlay::resized()
{
    const int w = juce::jmin (getWidth() - 24, 1040), h = 40;
    bandArea = juce::Rectangle<int> ((getWidth() - w) / 2, bandAtTop ? 84 : getHeight() - h - 14, w, h);

    auto b = bandArea.reduced (10, 7);
    doneButton.setBounds (b.removeFromRight (58));
    b.removeFromRight (6);
    windowButton.setBounds (b.removeFromRight (96));
    b.removeFromRight (12);
    scopeSelected.setBounds (b.removeFromRight (104));
    scopeBank.setBounds (b.removeFromRight (86));
    scopeRack.setBounds (b.removeFromRight (78));
    b.removeFromRight (10);
    hint.setBounds (b);
}

bool MidiMapOverlay::hitTest (int x, int y)
{
    if (bandArea.contains (x, y))
        return true;

    for (auto& s : shown)
        if (s.area.contains (x, y))
            return true;

    return false;   // everything else goes to the window below
}

void MidiMapOverlay::mouseDown (const juce::MouseEvent& e)
{
    // the smallest target under the mouse (a parameter row inside a slot...)
    const Shown* hit = nullptr;
    for (auto& s : shown)
        if (s.area.contains (e.getPosition()) && (hit == nullptr || s.area.getWidth() * s.area.getHeight() < hit->area.getWidth() * hit->area.getHeight()))
            hit = &s;

    if (hit == nullptr)
        return;

    if (e.mods.isPopupMenu())
    {
        showMenu (hit->target);
        return;
    }

    const auto t = asLearnTarget (hit->target);
    if (midi.learnRackMode == MidiTarget::bankRack && t.rackMode != MidiTarget::bankRack && hit->target.isRackTarget())
    {
        notice = "This rack is not in the controller's bank: mapped to the rack itself";
        noticeUntil = juce::Time::getMillisecondCounter() + 2500;
    }

    midi.setLearnTarget (t);
    grabKeyboardFocus();
    updateHint();
    repaint();
}

void MidiMapOverlay::showMenu (const MidiTarget& fixed)
{
    juce::PopupMenu m;
    const auto ids = mappingsShownAt (fixed);
    m.addSectionHeader (midi.describeTarget (fixed));

    for (auto id : ids)
        if (auto* map = midi.findMapping (id))
            m.addItem (juce::String::fromUTF8 ("Remove ") + midi.describeSource (*map) + "  (" + map->deviceName + ")",
                       [this, id] { midi.removeMapping (id); });

    if (ids.empty())
        m.addItem ("Not mapped", false, false, nullptr);

    m.addSeparator();
    m.addItem ("Map this control", [this, fixed] { midi.setLearnTarget (asLearnTarget (fixed)); });
    m.addItem ("MIDI window...", [this] { if (onOpenWindow) onOpenWindow(); });
    m.showMenuAsync (juce::PopupMenu::Options().withParentComponent (getTopLevelComponent()));
}

bool MidiMapOverlay::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        if (midi.getLearnTarget().isValid())
            midi.setLearnTarget ({});
        else if (onDone)
            onDone();
        return true;
    }

    if ((k == juce::KeyPress::backspaceKey || k == juce::KeyPress::deleteKey) && midi.getLearnTarget().isValid())
    {
        const auto& l = midi.getLearnTarget();
        auto* r = midi.resolveRack (l);
        auto fixed = l;
        if (l.isRackTarget() && r != nullptr) { fixed.rackMode = MidiTarget::fixedRack; fixed.rack = r->getId(); }

        for (auto id : mappingsShownAt (fixed))
            midi.removeMapping (id);
        midi.removeMappingsFor (l);   // also mappings of a bank strip / the selected rack not shown here
        return true;
    }

    return false;
}

} // namespace stagerack
