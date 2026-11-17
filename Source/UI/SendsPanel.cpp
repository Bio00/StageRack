#include "SendsPanel.h"
#include "Theme.h"
#include "ParamSlider.h"

namespace stagerack
{

static const juce::NormalisableRange<float>& sendRange()
{
    static const auto range = []
    {
        juce::NormalisableRange<float> r (SendsPanel::minDb, SendsPanel::maxDb);
        r.setSkewForCentre (-12.0f);
        return r;
    }();
    return range;
}

float SendsPanel::dbToPos (float db) noexcept
{
    return db <= Rack::sendOffDb ? 0.0f : sendRange().convertTo0to1 (juce::jlimit (minDb, maxDb, db));
}

float SendsPanel::posToDb (float pos) noexcept
{
    return pos <= 0.005f ? Rack::sendOffDb : sendRange().convertFrom0to1 (juce::jlimit (0.0f, 1.0f, pos));
}

static juce::String letterOf (int letter)
{
    return juce::String::charToString ((juce::juce_wchar) ('A' + letter - 1));
}

SendsPanel::SendsPanel (Session& s, EditController& e, Rack& r) : session (s), edit (e), rack (r)
{
    update();
}

bool SendsPanel::update()
{
    auto& engine = session.getEngine();
    std::vector<int> now;

    const auto shownMask = session.sendsShown (rack);   // the view (session or this rack) may hide some

    if (engine.isRoutingEnabled())
        for (int k = 1; k <= Rack::maxBuses; ++k)
            if (auto* bus = engine.findBus (k); bus != nullptr && bus != &rack && ! bus->isFolder && engine.canSendTo (rack, k)
                                                && (shownMask & (1 << (k - 1))) != 0)
                now.push_back (k);   // a bus that feeds this rack (loop) gets no send

    const bool countChanged = now.size() != letters.size();
    bool changed = now != letters;
    letters = std::move (now);
    shown.resize (letters.size(), -1000.0f);

    for (size_t i = 0; i < letters.size(); ++i)
    {
        auto& snd = rack.sends[(size_t) letters[i] - 1];
        const auto v = snd.db.load() + (snd.pre.load() ? 1000.0f : 0.0f);
        changed = changed || v != shown[i];
        shown[i] = v;
    }

    if (changed)
        repaint();

    return countChanged;
}

SendsPanel::Grid SendsPanel::grid (int width) const noexcept
{
    const auto n = juce::jmax (1, (int) letters.size());

    if (! usesKnobs (width))
        return { 1, fitHeight > 0 ? juce::jlimit (barH, 22, fitHeight / n) : barH };

    // the most columns that fit; with room to spare, the column count that gives the biggest knobs
    const bool mini = miniKnobs (width);
    const auto baseH = cellHFor (width), label = mini ? 11 : 15, side = mini ? 2 : 8;
    const auto maxCols = juce::jmax (1, width / cellWFor (width));
    auto knobSize = [=] (int cols, int cellH) { return juce::jmin (110, juce::jmin (width / cols - side, cellH - label)); };
    Grid best { maxCols, baseH };

    if (fitHeight <= 0)
        return best;

    auto bestSize = knobSize (maxCols, baseH);

    for (int c = 1; c <= maxCols; ++c)
    {
        const auto rows = (n + c - 1) / c;
        const auto h = fitHeight / rows;

        if (h < baseH)
            continue;

        if (const auto size = knobSize (c, h); size > bestSize)
        {
            bestSize = size;
            best = { c, juce::jmin (h, size + label + 2) };
        }
    }

    return best;
}

int SendsPanel::getContentHeight (int width) const noexcept
{
    const auto n = (int) letters.size();
    const auto g = grid (width);
    return ((n + g.cols - 1) / g.cols) * g.cellH;
}

juce::Rectangle<int> SendsPanel::cellBounds (int index) const
{
    const auto g = grid (getWidth());

    if (! knobs())
        return { 0, index * g.cellH, getWidth(), g.cellH };

    const auto w = (float) getWidth() / (float) g.cols;
    const auto col = index % g.cols, row = index / g.cols;
    return juce::Rectangle<float> (w * (float) col, (float) (row * g.cellH), w, (float) g.cellH).toNearestInt();
}

int SendsPanel::cellAt (juce::Point<int> p) const
{
    for (int i = 0; i < (int) letters.size(); ++i)
        if (cellBounds (i).contains (p))
            return i;
    return -1;
}

static juce::String valueText (float db)
{
    return db <= Rack::sendOffDb ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e"))
                                 : juce::String (db, db > -10.0f ? 1 : 0);
}

void SendsPanel::paint (juce::Graphics& g)
{
    for (int i = 0; i < (int) letters.size(); ++i)
    {
        if (knobs()) paintKnob (g, i);
        else         paintBar (g, i);
    }
}

void SendsPanel::paintBar (juce::Graphics& g, int i)
{
    const auto letter = letters[(size_t) i];
    auto* bus = session.getEngine().findBus (letter);
    auto& snd = rack.sends[(size_t) letter - 1];
    const auto db = snd.db.load();
    auto b = cellBounds (i).toFloat().reduced (0.0f, 1.0f);
    const auto colour = bus != nullptr ? bus->colour : theme::accent;
    const bool active = hoverCell == i || dragCell == i;

    g.setColour (theme::well);
    g.fillRoundedRectangle (b, 3.0f);

    if (db > Rack::sendOffDb)
    {
        g.setColour (colour.withAlpha (active ? 0.6f : 0.42f));
        g.fillRoundedRectangle (b.withWidth (juce::jmax (3.0f, b.getWidth() * dbToPos (db))), 3.0f);
    }

    auto text = b.toNearestInt().reduced (4, 0);

    // letter in the bus colour, then the bus name; the value (and PRE) on the right
    g.setFont (theme::font (9.5f, true));
    g.setColour (colour);
    g.drawText (letterOf (letter), text.removeFromLeft (10), juce::Justification::centredLeft);

    const bool narrow = getWidth() < 90;
    const bool showValue = ! narrow || active;

    if (snd.pre.load())
    {
        auto tag = text.removeFromRight (20).reduced (0, 1).toFloat();
        g.setColour (theme::amber.withAlpha (0.85f));
        g.fillRoundedRectangle (tag, 2.0f);
        g.setColour (juce::Colours::black);
        g.setFont (theme::font (8.0f, true));
        g.drawText ("PRE", tag, juce::Justification::centred);
        text.removeFromRight (2);
    }

    g.setFont (theme::font (9.5f));
    g.setColour (theme::text.withAlpha (0.85f));

    if (showValue)
        g.drawText (valueText (db), text.removeFromRight (narrow ? text.getWidth() : 30), juce::Justification::centredRight);

    if (! narrow || ! showValue)
    {
        g.setColour (theme::text);
        g.drawFittedText (bus != nullptr ? bus->name : juce::String(), text, juce::Justification::centredLeft, 1, 0.7f);
    }
}

void SendsPanel::paintKnob (juce::Graphics& g, int i)
{
    // Ableton-like: a small knob in the bus colour with its letter, the bus name below
    // (the value while hovering / dragging), PRE tag in the corner
    const auto letter = letters[(size_t) i];
    auto* bus = session.getEngine().findBus (letter);
    auto& snd = rack.sends[(size_t) letter - 1];
    const auto db = snd.db.load();
    const auto colour = bus != nullptr ? bus->colour : theme::accent;
    const bool active = hoverCell == i || dragCell == i;
    auto cell = cellBounds (i).reduced (miniKnobs (getWidth()) ? 1 : 2, 1);
    const bool small = cell.getHeight() < 40 || cell.getWidth() < 40;

    auto label = cell.removeFromBottom (small ? 10 : 13);
    const auto size = (float) juce::jmin (cell.getWidth() - (small ? 2 : 8), cell.getHeight() - 2);
    const auto knob = juce::Rectangle<float> (size, size).withCentre (cell.toFloat().getCentre());
    const auto centre = knob.getCentre();
    const auto radius = size * 0.5f;
    constexpr auto start = juce::MathConstants<float>::pi * 1.25f, end = juce::MathConstants<float>::pi * 2.75f;

    g.setColour (theme::well);
    g.fillEllipse (knob.reduced (3.0f));

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius - juce::jmax (2.0f, size * 0.04f), radius - juce::jmax (2.0f, size * 0.04f), 0.0f, start, end, true);
    g.setColour (theme::outline);
    g.strokePath (track, juce::PathStrokeType (juce::jmax (3.0f, size * 0.06f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (db > Rack::sendOffDb)
    {
        const auto angle = start + (end - start) * dbToPos (db);
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, radius - juce::jmax (2.0f, size * 0.04f), radius - juce::jmax (2.0f, size * 0.04f), 0.0f, start, angle, true);
        g.setColour (active ? colour.brighter (0.2f) : colour);
        g.strokePath (arc, juce::PathStrokeType (juce::jmax (3.0f, size * 0.06f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    g.setColour (db > Rack::sendOffDb ? colour : theme::textDim);
    g.setFont (theme::font (juce::jlimit (8.5f, 17.0f, size * 0.3f), true));
    g.drawText (letterOf (letter), knob.reduced (radius * 0.45f), juce::Justification::centred);

    if (snd.pre.load())
    {
        if (small)
        {
            g.setColour (theme::amber.withAlpha (0.9f));   // no room for the tag: an amber dot
            g.fillEllipse ((float) cell.getRight() - 5.0f, (float) cell.getY(), 5.0f, 5.0f);
        }
        else
        {
            auto tag = juce::Rectangle<float> ((float) cell.getRight() - 19.0f, (float) cell.getY(), 19.0f, 10.0f);
            g.setColour (theme::amber.withAlpha (0.85f));
            g.fillRoundedRectangle (tag, 2.0f);
            g.setColour (juce::Colours::black);
            g.setFont (theme::font (7.5f, true));
            g.drawText ("PRE", tag, juce::Justification::centred);
        }
    }

    g.setFont (theme::font (size > 60.0f ? 10.5f : (small ? 8.0f : 9.0f), active));
    g.setColour (active ? theme::text : theme::text.withAlpha (0.8f));
    g.drawFittedText (active ? valueText (db) + (small ? juce::String() : juce::String (" dB")) : (bus != nullptr ? bus->name : juce::String()),
                      label.expanded (small ? 2 : 0, 0), juce::Justification::centred, 1, 0.6f);
}

void SendsPanel::setLevel (int letter, float db, const juce::ModifierKeys& mods, float relativeDelta)
{
    auto& engine = session.getEngine();
    const auto k = (size_t) letter - 1;
    engine.setSend (rack, letter, db);

    // Alt: same level on the selected racks (Alt+Shift: relative); edit-group members follow relatively
    for (auto* r : edit.linkedRacks (rack, mods))
    {
        if (! engine.canSendTo (*r, letter))
            continue;

        auto& snd = r->sends[k];

        if (mods.isShiftDown() || ! edit.linkedBySelection (rack, *r, mods))
        {
            if (snd.isOn() && relativeDelta != 0.0f)
                engine.setSend (*r, letter, juce::jlimit (minDb, maxDb, snd.db.load() + relativeDelta));
            else if (db <= Rack::sendOffDb)
                engine.setSend (*r, letter, Rack::sendOffDb);
        }
        else
        {
            engine.setSend (*r, letter, db);
        }
    }

    update();
}

void SendsPanel::mouseDown (const juce::MouseEvent& e)
{
    const auto cell = cellAt (e.getPosition());

    if (cell < 0)
        return;

    if (e.mods.isPopupMenu())
    {
        showMenu (cell);
        return;
    }

    dragCell = cell;
    dragMods = e.mods;
    dragLastDb = rack.sends[(size_t) letters[(size_t) cell] - 1].db.load();
    dragStartPos = dragPos = dbToPos (dragLastDb);
    dragLast = e.position;
    altClick = e.mods.isAltDown();
    repaint();
}

void SendsPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragCell < 0 || dragCell >= (int) letters.size())
        return;

    if (e.getDistanceFromDragStart() > 2)
        altClick = false;

    if (altClick)
        return;

    // knobs: up/down over ~160 px; bars: across their width. Cmd/Shift: 5x finer, also mid-drag
    // (incremental, so the value doesn't jump when the key changes)
    const auto fine = ParamSlider::isFine (e.mods) ? 1.0f / ParamSlider::fineFactor : 1.0f;
    const auto d = e.position - dragLast;
    dragLast = e.position;
    dragPos = juce::jlimit (0.0f, 1.0f, dragPos + (knobs() ? -d.y / (160.0f * fine)
                                                           : d.x / (fine * (float) juce::jmax (40, getWidth()))));
    const auto db = posToDb (dragPos);
    const auto delta = (db > Rack::sendOffDb && dragLastDb > Rack::sendOffDb) ? db - dragLastDb : 0.0f;
    setLevel (letters[(size_t) dragCell], db, dragMods, delta);
    dragLastDb = db;
}

void SendsPanel::mouseUp (const juce::MouseEvent& e)
{
    if (dragCell >= 0 && dragCell < (int) letters.size() && altClick && e.getDistanceFromDragStart() <= 2)
        setLevel (letters[(size_t) dragCell], Rack::sendOffDb, dragMods);

    if (dragCell >= 0)
        session.markDirty();

    altClick = false;

    dragCell = -1;
    repaint();
}

void SendsPanel::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto cell = cellAt (e.getPosition());

    if (cell < 0)
        return;

    const auto letter = letters[(size_t) cell];
    const bool on = rack.sends[(size_t) letter - 1].isOn();
    setLevel (letter, on ? Rack::sendOffDb : 0.0f, e.mods);
    session.markDirty();
}

void SendsPanel::mouseMove (const juce::MouseEvent& e)
{
    if (const auto cell = cellAt (e.getPosition()); cell != hoverCell)
    {
        hoverCell = cell;
        repaint();

        if (cell >= 0)
        {
            const auto letter = letters[(size_t) cell];
            auto* bus = session.getEngine().findBus (letter);
            setTooltip ("Send to Bus " + letterOf (letter) + (bus != nullptr ? " (" + bus->name + ")" : juce::String())
                        + ": drag (Shift fine), double-click off/0 dB, right-click pre/post  (Alt: all selected racks)");
        }
    }
}

void SendsPanel::mouseExit (const juce::MouseEvent&)
{
    hoverCell = -1;
    repaint();
}

juce::PopupMenu SendsPanel::sendsShownMenu (Session& session, EditController& edit, Rack& rack)
{
    auto& engine = session.getEngine();
    const int id = rack.getId();
    const auto viewMask = session.getRackView().sendsMask;
    const auto mine = rack.uiSendsShown;
    const auto shown = mine >= 0 ? mine : viewMask;

    // applied when chosen, to this rack (Alt: every selected rack too); the rack is found again by id
    auto apply = [&session, &edit, id] (std::function<int (int current)> change)
    {
        auto* r = session.getEngine().findRack (id);
        if (r == nullptr) return;
        auto all = edit.linkedRacks (*r, juce::ModifierKeys::getCurrentModifiersRealtime());
        all.add (r);
        for (auto* x : all)
            session.setRackSendsShown (*x, change (x->uiSendsShown >= 0 ? x->uiSendsShown : session.getRackView().sendsMask));
    };

    juce::PopupMenu m;
    m.addItem ("As the view (VIEW menu)", true, mine < 0, [apply] { apply ([] (int) { return -1; }); });
    m.addItem ("All", true, mine >= 0 && (mine & 0xff) == 0xff, [apply] { apply ([] (int) { return 0xff; }); });
    m.addItem ("None", true, mine == 0, [apply] { apply ([] (int) { return 0; }); });
    m.addSectionHeader ("Buses");

    for (int k = 1; k <= Rack::maxBuses; ++k)
    {
        auto* bus = engine.findBus (k);
        if (bus == nullptr || bus == &rack || bus->isFolder)
            continue;

        const auto bit = 1 << (k - 1);
        const auto on = rack.sends[(size_t) k - 1].isOn();
        m.addItem ("Bus " + letterOf (k) + (bus->name == "Bus " + letterOf (k) ? juce::String() : "  " + bus->name)
                       + (on && (shown & bit) == 0 ? "   (on, hidden)" : ""),
                   true, (shown & bit) != 0, [apply, bit] { apply ([bit] (int cur) { return cur ^ bit; }); });
    }

    if (! session.viewFor (rack).sends)
        m.addItem ("(this rack's view hides the sends)", false, false, nullptr);

    return m;
}

void SendsPanel::showMenu (int cell)
{
    const auto letter = letters[(size_t) cell];
    const auto mods = juce::ModifierKeys::getCurrentModifiers();
    juce::Component::SafePointer<SendsPanel> safe (this);
    const bool pre = rack.sends[(size_t) letter - 1].pre.load();

    auto setPre = [safe, letter, mods] (bool p)
    {
        if (safe == nullptr) return;
        auto& self = *safe;
        auto all = self.edit.linkedRacks (self.rack, mods);
        all.add (&self.rack);
        for (auto* r : all)
            r->sends[(size_t) letter - 1].pre = p;
        self.update();
        self.session.markDirty();
    };
    auto setDb = [safe, letter, mods] (float db)
    {
        if (safe == nullptr) return;
        safe->setLevel (letter, db, mods);
        safe->session.markDirty();
    };

    juce::PopupMenu m;
    m.addSectionHeader ("Send to Bus " + letterOf (letter));
    m.addItem ("Post-fader", true, ! pre, [setPre] { setPre (false); });
    m.addItem ("Pre-fader (ignores fader and automix, follows mute)", true, pre, [setPre] { setPre (true); });
    m.addSeparator();
    m.addItem ("0 dB", [setDb] { setDb (0.0f); });
    m.addItem ("-6 dB", [setDb] { setDb (-6.0f); });
    m.addItem ("Off", [setDb] { setDb (Rack::sendOffDb); });
    m.addSeparator();
    m.addSubMenu ("Sends shown", sendsShownMenu (session, edit, rack));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (cellBounds (cell))));
}

} // namespace stagerack
