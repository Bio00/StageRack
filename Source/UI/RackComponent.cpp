#include "RackComponent.h"
#include "PluginWindow.h"
#include "PluginThumbnail.h"
#include "PluginMeters.h"
#include "PluginBrowser.h"
#include <map>
#include "Theme.h"

namespace stagerack
{

static juce::String shortFormat (const juce::String& f)
{
    if (f == "AudioUnit") return "AU";
    return f;
}

//==============================================================================
static bool altDown()   { return juce::ModifierKeys::getCurrentModifiers().isAltDown(); }

/** Menu item with its keyboard shortcut shown right-aligned. */
static juce::PopupMenu::Item menuItem (const juce::String& text, const char* shortcutUtf8, std::function<void()> action)
{
    juce::PopupMenu::Item item (text);
    item.shortcutKeyDescription = juce::String::fromUTF8 (shortcutUtf8);
    item.action = std::move (action);
    return item;
}

//==============================================================================
/** Default parameters shown on the rack: the ones that look like main controls (mix, output,
    threshold, strength...), skipping sidechain/MIDI/bypass and per-band housekeeping flags.
    Falls back to the first automatable parameters. Kept in the plugin's own order. */
static juce::Array<int> defaultShownParams (juce::AudioPluginInstance& p, int maxCount = 12)
{
    static const char* preferred[] = { "mix", "dry/wet", "wet", "strength", "amount", "threshold", "output", "gain",
                                       "frequency", "freq", " q",
                                       "drive", "depth", "decay", "time", "reduction", "ratio", "level", "volume",
                                       "sensitivity", "intensity", "tone" };
    static const char* skipped[]   = { "bypass", "sidechain", "side chain", "midi", "used", "enabled", "solo",
                                       "oversampl", "latency", "lookahead", "program", "preset" };

    const auto& params = p.getParameters();
    std::vector<std::pair<int, int>> scored;   // (score, index)

    for (int i = 0; i < params.size() && i < 512; ++i)
    {
        auto* prm = params[i];
        const auto name = prm->getName (64).toLowerCase();

        if (! prm->isAutomatable() || name.isEmpty())
            continue;

        bool skip = false;
        for (auto* k : skipped)
            skip = skip || name.contains (k);

        if (skip)
            continue;

        int score = 0;
        for (int k = 0; k < (int) std::size (preferred); ++k)
            if (name.contains (preferred[k]))
                score = juce::jmax (score, 100 - k);

        scored.push_back ({ score, i });
    }

    std::stable_sort (scored.begin(), scored.end(), [] (auto& x, auto& y) { return x.first > y.first; });

    // Variety first: at most 2 parameters of the same kind ("Band 3 Threshold" and
    // "Band 5 Threshold" are the same kind), then fill up with the rest.
    auto kindOf = [&params] (int i) { return params[i]->getName (64).toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz "); };
    std::map<juce::String, int> perKind;
    juce::Array<int> result;

    for (auto& [score, i] : scored)
        if (result.size() < maxCount && ++perKind[kindOf (i)] <= 2)
            result.add (i);

    for (auto& [score, i] : scored)
        if (result.size() < maxCount && ! result.contains (i))
            result.add (i);

    result.sort();
    return result;
}

/** One plugin parameter shown on the rack: drag (shift = fine), wheel, double-click = default,
    click toggles on/off parameters. Looks the parameter up on every use, so it can never
    outlive the plugin. */
class ParamRow final : public juce::Component
{
public:
    ParamRow (EditController& e, Rack& r, int slotIndex, int paramIndex) : edit (e), rack (r), slot (slotIndex), index (paramIndex) {}

    int getParamIndex() const noexcept { return index; }

    /** Repaints only when the value moved (called ~30x/s). */
    void refresh()
    {
        if (auto* prm = param(); prm != nullptr && prm->getValue() != shownValue)
        {
            shownValue = prm->getValue();
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto* prm = param();

        if (prm == nullptr)
            return;

        auto b = getLocalBounds().toFloat().reduced (0.0f, 1.0f);
        const auto v = juce::jlimit (0.0f, 1.0f, prm->getValue());
        const auto colour = rack.colour;

        g.setColour (theme::well);
        g.fillRoundedRectangle (b, 3.0f);

        if (isToggle (*prm))
        {
            const bool on = v >= 0.5f;
            auto pill = b.removeFromRight (30.0f).reduced (2.0f, 1.0f);
            g.setColour (on ? colour : theme::outline);
            g.fillRoundedRectangle (pill, pill.getHeight() / 2.0f);
            g.setColour (on ? juce::Colours::black : theme::textDim);
            g.setFont (theme::font (9.0f, true));
            g.drawText (on ? "ON" : "OFF", pill, juce::Justification::centred);
        }
        else
        {
            g.setColour (colour.withAlpha (isMouseOverOrDragging() ? 0.55f : 0.38f));
            g.fillRoundedRectangle (b.withWidth (juce::jmax (3.0f, b.getWidth() * v)), 3.0f);
        }

        auto text = getLocalBounds().reduced (5, 0);
        const bool toggle = isToggle (*prm);

        if (toggle)
            text.removeFromRight (30);

        // narrow slots: name only, the value replaces it while hovering/dragging
        const bool narrow = getWidth() < 120;
        const bool showValueOnly = narrow && ! toggle && (isMouseOverOrDragging() || dragging);

        g.setColour (theme::text);
        g.setFont (theme::font (narrow ? 9.5f : 10.0f, true));

        if (showValueOnly)
            g.drawFittedText (prm->getCurrentValueAsText(), text, juce::Justification::centred, 1, 0.7f);
        else
            g.drawFittedText (prm->getName (32), text, juce::Justification::centredLeft, 1, 0.75f);

        if (! toggle && ! narrow)
        {
            g.setColour (theme::text.withAlpha (0.8f));
            g.setFont (theme::font (10.0f));
            g.drawText (prm->getCurrentValueAsText().substring (0, 12), text, juce::Justification::centredRight, true);
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* prm = param();

        if (prm == nullptr || e.mods.isPopupMenu())
            return;

        if (isToggle (*prm))
        {
            set (*prm, prm->getValue() >= 0.5f ? 0.0f : 1.0f, e.mods);
            return;
        }

        startValue = prm->getValue();
        dragMods = e.mods;
        prm->beginChangeGesture();

        for (auto* other : linkedParams (dragMods))
            other->beginChangeGesture();

        dragging = true;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (auto* prm = param(); prm != nullptr && dragging)
        {
            const auto range = e.mods.isShiftDown() ? 4.0f * (float) getWidth() : (float) getWidth();
            const auto v = juce::jlimit (0.0f, 1.0f, startValue + (float) e.getDistanceFromDragStartX() / range);
            prm->setValueNotifyingHost (v);

            for (auto* other : linkedParams (dragMods))
                other->setValueNotifyingHost (v);

            refresh();
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (auto* prm = param(); prm != nullptr && dragging)
        {
            prm->endChangeGesture();

            for (auto* other : linkedParams (dragMods))
                other->endChangeGesture();
        }

        dragging = false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (auto* prm = param(); prm != nullptr && ! isToggle (*prm))
            set (*prm, prm->getDefaultValue(), e.mods);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (auto* prm = param(); prm != nullptr && ! isToggle (*prm))
            set (*prm, juce::jlimit (0.0f, 1.0f, prm->getValue() + w.deltaY * (e.mods.isShiftDown() ? 0.01f : 0.05f)), e.mods);
    }

private:
    juce::AudioProcessorParameter* param() const
    {
        if (auto* p = rack.getPlugin (slot))
            if (juce::isPositiveAndBelow (index, p->getParameters().size()))
                return p->getParameters()[index];

        return nullptr;
    }

    static bool isToggle (const juce::AudioProcessorParameter& p)
    {
        return p.isBoolean() || (p.isDiscrete() && p.getNumSteps() == 2);
    }

    /** Alt held on a selected rack: the same parameter of the same plugin in the other selected racks. */
    juce::Array<juce::AudioProcessorParameter*> linkedParams (const juce::ModifierKeys& mods) const
    {
        juce::Array<juce::AudioProcessorParameter*> result;

        for (auto& [r, s] : edit.linkedSlots (rack, slot, mods))
            if (auto* p = r->getPlugin (s); p != nullptr && juce::isPositiveAndBelow (index, p->getParameters().size()))
                result.add (p->getParameters()[index]);

        return result;
    }

    void set (juce::AudioProcessorParameter& p, float v, const juce::ModifierKeys& mods)
    {
        auto targets = linkedParams (mods);
        targets.add (&p);

        for (auto* t : targets)
        {
            t->beginChangeGesture();
            t->setValueNotifyingHost (v);
            t->endChangeGesture();
        }

        refresh();
    }

    EditController& edit;
    Rack& rack;
    const int slot, index;
    float shownValue = -1.0f, startValue = 0.0f;
    bool dragging = false;
    juce::ModifierKeys dragMods;
};

class SlotComponent final : public juce::Component,
                            public juce::DragAndDropTarget
{
public:
    SlotComponent (Session& s, EditController& e, Rack& r, int i) : session (s), edit (e), rack (r), index (i)
    {
        addChildComponent (resizer);
    }

    /** Orientation of the owning rack: the resize handle sits on the right (horizontal) or bottom edge. */
    void setVertical (bool v)
    {
        vertical = v;
        resized();
    }

    std::function<void()> onSlotResized;   // owner re-lays out its slots

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        auto* p = rack.getPlugin (index);
        const auto& sl = rack.getSlot (index);
        const bool bypassed = sl.bypassed.load();
        const bool selected = edit.isSlotSelected (rack.getId(), index);

        g.setColour (dragOver ? theme::accent.withAlpha (0.25f) : (isMouseOver (true) ? theme::slotHover : theme::slot));
        g.fillRoundedRectangle (b, 5.0f);

        if (p == nullptr)
        {
            g.setColour (selected ? theme::accent : theme::outline);
            g.drawRoundedRectangle (b, 5.0f, selected ? 2.0f : 1.0f);
            g.setColour (theme::textDim.withAlpha (isMouseOver() ? 1.0f : 0.5f));
            g.setFont (theme::font (18.0f));
            g.drawText ("+", b, juce::Justification::centred);
            return;
        }

        // --- header: power button, name, view toggle
        const auto header = headerArea();
        const bool narrowHeader = header.getWidth() < 64;

        powerArea = header.withWidth (18.0f).reduced (1.0f);
        g.setColour (bypassed ? theme::outline : theme::green);
        drawPowerIcon (g, powerArea, bypassed ? theme::textDim : juce::Colour (0xff0d1f14), ! bypassed);

        viewArea = narrowHeader ? juce::Rectangle<float>() : header.withLeft (header.getRight() - 18.0f).reduced (2.0f);
        if (! viewArea.isEmpty())
        {
            g.setColour (isMouseOver (true) ? theme::text : theme::textDim);
            drawViewIcon (g, viewArea, (sl.viewMode + 1) % numViews);
        }

        g.setColour (bypassed ? theme::textDim : theme::text);
        g.setFont (theme::font (12.0f, true));
        auto nameArea = header.withTrimmedLeft (21.0f).withTrimmedRight (viewArea.isEmpty() ? 0.0f : 20.0f);
        g.drawFittedText (p->getName(), nameArea.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);

        // --- body
        const auto body = bodyArea();

        if (sl.viewMode == 1)
            paintMiniView (g, body, *p, bypassed);
        else if (sl.viewMode == 2)
            paintMeters (g, body, *p);

        // --- footer: format, cpu, hidden parameters
        if (showFooter)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (10.0f));
            const auto foot = b.reduced (6, 3).removeFromBottom (12);
            juce::String left = shortFormat (p->getPluginDescription().pluginFormatName);

            if (sl.viewMode == 0 && hiddenRows > 0)
                left << "  +" << hiddenRows << " par.";
            else if (sl.viewMode == 2 && meters.size() > shownMeters)
                left << "  +" << (meters.size() - shownMeters) << " meters";

            g.drawText (left, foot, juce::Justification::centredLeft);
            g.drawText (juce::String (sl.cpuMs.load(), 2) + " ms", foot, juce::Justification::centredRight);
        }

        if (bypassed)
        {
            // dim everything but the header while the plugin is off
            g.setColour (theme::slot.withAlpha (0.55f));
            g.fillRect (body);
            g.setColour (theme::amber);
            g.setFont (theme::font (9.0f, true));
            g.drawText ("OFF", body.withHeight (14.0f), juce::Justification::centredRight);
        }

        g.setColour (selected ? theme::accent : (bypassed ? theme::outline : rack.colour.withAlpha (0.8f)));
        g.drawRoundedRectangle (b, 5.0f, selected ? 2.4f : 1.2f);
    }

    /** Repaints only when what the slot shows has changed (called ~30x/s). */
    void refreshStats()
    {
        auto* p = rack.getPlugin (index);
        const auto& sl = rack.getSlot (index);
        const int cpuHundredths = p != nullptr ? juce::roundToInt (sl.cpuMs.load() * 100.0f) : -1;
        const bool bypassed = sl.bypassed.load();
        const bool selected = edit.isSlotSelected (rack.getId(), index);

        // teach the browser this plugin's latency (whenever it is loaded or changes)
        if (p != nullptr && (p != shownPlugin || p->getLatencySamples() != shownLatency))
        {
            shownLatency = p->getLatencySamples();
            session.getPluginManager().recordLatency (p->getPluginDescription(), shownLatency, rack.getSampleRate());
        }

        if (p != shownPlugin || sl.viewMode != shownView)
        {
            shownPlugin = p;
            shownView = sl.viewMode;
            syncRows();
        }

        const auto thumbVersion = p != nullptr && sl.viewMode == 1 ? PluginThumbnails::get().find (p).version : 0;

        bool metersMoved = false;

        if (p != nullptr && sl.viewMode == 2)
        {
            for (int i = 0; i < meters.size(); ++i)
            {
                const auto v = readMeter (*p, meters.getReference (i));

                if (i >= meterValues.size())     { meterValues.add (v); metersMoved = true; }
                else if (std::abs (meterValues[i] - v) > 0.002f) { meterValues.set (i, v); metersMoved = true; }
            }
        }

        if (metersMoved)
            repaint (bodyArea().toNearestInt());

        if (cpuHundredths != shownCpu || bypassed != shownBypass || selected != shownSelected || thumbVersion != shownThumb)
        {
            shownCpu = cpuHundredths;
            shownBypass = bypassed;
            shownSelected = selected;
            shownThumb = thumbVersion;
            repaint();
        }

        for (auto* r : rows)
            if (r->isVisible())
                r->refresh();
    }

    /** Rebuilds the parameter rows (plugin or view changed, or other parameters picked). */
    void syncRows()
    {
        rows.clear();
        auto& sl = rack.getSlot (index);

        if (auto* p = rack.getPlugin (index))
        {
            if (! sl.paramsChosen)
                sl.shownParams = defaultShownParams (*p);

            if (meterPlugin != p)
            {
                meterPlugin = p;
                meters = findMeters (*p);
                meterValues.clearQuick();
            }

            if (sl.viewMode == 0)
                for (auto i : sl.shownParams)
                    addChildComponent (rows.add (new ParamRow (edit, rack, index, i)));
            else if (! PluginThumbnails::get().find (p).image.isValid())
                PluginThumbnails::get().requestOffscreen (*p);   // first picture for the mini view
        }

        resizer.toFront (false);
        resized();
        repaint();
    }

    void resized() override
    {
        auto* p = rack.getPlugin (index);
        resizer.setVisible (p != nullptr);

        if (vertical)
            resizer.setBounds (0, getHeight() - 5, getWidth(), 5);
        else
            resizer.setBounds (getWidth() - 5, 0, 5, getHeight());

        // parameters in a grid: one column per ~104 px, as many rows as fit (column by column)
        constexpr int rowH = 17, colW = 104;
        auto body = bodyArea().toNearestInt();
        const int cols = juce::jmax (1, body.getWidth() / colW);
        int rowsFit = juce::jmax (0, body.getHeight() / rowH);

        showFooter = body.getHeight() >= 12 + (rows.isEmpty() ? 0 : rowH);

        if (showFooter && ! rows.isEmpty() && cols * rowsFit < rows.size() + cols)
            rowsFit = juce::jmax (0, (body.getHeight() - 12) / rowH);   // footer takes the last row

        const int capacity = cols * rowsFit;
        hiddenRows = juce::jmax (0, rows.size() - capacity);
        const int colWidth = cols > 0 ? body.getWidth() / cols : body.getWidth();

        for (int i = 0; i < rows.size(); ++i)
        {
            auto* r = rows[i];
            const bool visible = i < capacity;
            r->setVisible (visible);

            if (visible)
            {
                const int c = i / juce::jmax (1, rowsFit), row = i % juce::jmax (1, rowsFit);
                r->setBounds (body.getX() + c * colWidth, body.getY() + row * rowH,
                              colWidth - (c < cols - 1 ? 4 : 0), rowH - 2);
            }
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* p = rack.getPlugin (index);
        const bool onPower = p != nullptr && powerArea.expanded (3).contains (e.position);
        const bool onView = p != nullptr && ! viewArea.isEmpty() && viewArea.expanded (3).contains (e.position);

        // Alt on power/view of a selected rack: same change on that plugin in all selected racks.
        // The rack selection is kept, so it can be repeated.
        if ((onPower || onView) && ! e.mods.isPopupMenu() && e.mods.isAltDown() && edit.isRackSelected (rack.getId()))
        {
            if (onPower) toggleBypass (e.mods);
            else         toggleView (e.mods);
            return;
        }

        edit.selectSlot (rack.getId(), index);

        if (e.mods.isPopupMenu())
        {
            showMenu();
            return;
        }

        if (p == nullptr)
        {
            openBrowser();
            return;
        }

        if (onPower)
            toggleBypass();
        else if (onView)
            toggleView();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        auto* p = rack.getPlugin (index);

        if (p != nullptr && ! e.mods.isPopupMenu() && ! e.mouseWasDraggedSinceMouseDown()
            && ! powerArea.expanded (3).contains (e.position)
            && (viewArea.isEmpty() || ! viewArea.expanded (3).contains (e.position)))
            openEditor();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (rack.getPlugin (index) != nullptr && e.getDistanceFromDragStart() > 6)
            if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor (this))
                if (! dnd->isDragAndDropActive())
                    dnd->startDragging ("slot:" + juce::String (rack.getId()) + ":" + juce::String (index), this);
    }

    // Plugins can be dropped on any slot of any rack: move, or copy with Alt held.
    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        return d.description.toString().startsWith ("slot:");
    }

    void itemDragEnter (const SourceDetails&) override { dragOver = true; repaint(); }
    void itemDragExit (const SourceDetails&) override  { dragOver = false; repaint(); }

    void itemDropped (const SourceDetails& d) override
    {
        dragOver = false;
        const auto parts = juce::StringArray::fromTokens (d.description.toString(), ":", "");
        edit.dropPlugin (parts[1].getIntValue(), parts[2].getIntValue(), rack.getId(), index, altDown());
        repaint();
    }

private:
    //==============================================================================
    /** Handle on the slot's trailing edge: drag to widen/narrow, double-click = default size. */
    class SlotResizer final : public juce::Component
    {
    public:
        explicit SlotResizer (SlotComponent& o) : owner (o) {}

        void mouseEnter (const juce::MouseEvent&) override
        {
            setMouseCursor (owner.vertical ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::LeftRightResizeCursor);
            repaint();
        }

        void mouseExit (const juce::MouseEvent&) override { repaint(); }

        void mouseDown (const juce::MouseEvent&) override
        {
            startSize = owner.rack.getSlot (owner.index).uiSize;
            startLength = (float) juce::jmax (1, owner.vertical ? owner.getHeight() : owner.getWidth());
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const auto delta = (float) (owner.vertical ? e.getDistanceFromDragStartY() : e.getDistanceFromDragStartX());
            owner.rack.getSlot (owner.index).uiSize = juce::jlimit (0.4f, 6.0f, startSize * (startLength + delta) / startLength);

            if (owner.onSlotResized)
                owner.onSlotResized();
        }

        void mouseUp (const juce::MouseEvent&) override { owner.session.markDirty(); }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            owner.rack.getSlot (owner.index).uiSize = 1.0f;

            if (owner.onSlotResized)
                owner.onSlotResized();

            owner.session.markDirty();
        }

        void paint (juce::Graphics& g) override
        {
            if (isMouseOver() || isMouseButtonDown())
            {
                g.setColour (theme::accent.withAlpha (0.7f));
                g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (owner.vertical ? 10.0f : 1.0f, owner.vertical ? 1.0f : 10.0f), 2.0f);
            }
        }

    private:
        SlotComponent& owner;
        float startSize = 1.0f, startLength = 1.0f;
    };

    juce::Rectangle<float> headerArea() const
    {
        return getLocalBounds().toFloat().reduced (2.0f).reduced (5.0f, 4.0f).withHeight (18.0f);
    }

    juce::Rectangle<float> bodyArea() const
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f).reduced (6.0f, 4.0f);
        b.removeFromTop (21.0f);

        if (vertical) b.removeFromBottom (3.0f);
        else          b.removeFromRight (3.0f);

        return b;
    }

    static void drawPowerIcon (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour symbol, bool on)
    {
        g.fillEllipse (r);
        const auto c = r.getCentre();
        const auto rad = r.getWidth() * 0.27f;

        juce::Path arc;
        arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, 0.75f, juce::MathConstants<float>::twoPi - 0.75f, true);
        g.setColour (symbol);
        g.strokePath (arc, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.drawLine (c.x, c.y - rad - 1.0f, c.x, c.y - 0.5f, 1.5f);
        juce::ignoreUnused (on);
    }

    /** Icon of the view a click switches to: 0 = parameters (list), 1 = mini view (picture), 2 = meters (bars). */
    static void drawViewIcon (juce::Graphics& g, juce::Rectangle<float> r, int nextView)
    {
        r = r.reduced (1.5f);

        if (nextView == 2)
        {
            const float bw = r.getWidth() / 4.0f;
            const float heights[] = { 0.55f, 1.0f, 0.75f };

            for (int i = 0; i < 3; ++i)
                g.fillRoundedRectangle (r.getX() + bw * 0.25f + (float) i * bw * 1.3f, r.getBottom() - r.getHeight() * heights[i],
                                        bw, r.getHeight() * heights[i], 0.8f);
        }
        else if (nextView == 1)
        {
            g.drawRoundedRectangle (r, 2.0f, 1.2f);
            juce::Path m;
            m.startNewSubPath (r.getX() + 2.0f, r.getBottom() - 2.5f);
            m.lineTo (r.getX() + r.getWidth() * 0.45f, r.getY() + r.getHeight() * 0.45f);
            m.lineTo (r.getX() + r.getWidth() * 0.65f, r.getY() + r.getHeight() * 0.65f);
            m.lineTo (r.getRight() - 2.0f, r.getY() + 3.0f);
            g.strokePath (m, juce::PathStrokeType (1.2f));
        }
        else
        {
            for (int i = 0; i < 3; ++i)
                g.fillRoundedRectangle (r.getX(), r.getY() + 1.5f + (float) i * r.getHeight() / 3.0f, r.getWidth(), 1.6f, 0.8f);
        }
    }

    /** Mini view: the latest picture of the plugin's editor, or a card if there is none. */
    void paintMiniView (juce::Graphics& g, juce::Rectangle<float> body, juce::AudioPluginInstance& p, bool bypassed)
    {
        if (showFooter)
            body.removeFromBottom (13.0f);

        if (body.getHeight() < 8.0f)
            return;

        const auto entry = PluginThumbnails::get().find (&p);

        if (entry.image.isValid() && ! PluginThumbnails::looksBlank (entry.image))
        {
            const auto img = entry.image;
            const auto fitted = juce::RectanglePlacement (juce::RectanglePlacement::centred)
                                    .appliedTo (img.getBounds().toFloat(), body);
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clip;
            clip.addRoundedRectangle (fitted, 3.0f);
            g.reduceClipRegion (clip);
            g.setOpacity (bypassed ? 0.4f : 1.0f);
            g.drawImage (img, fitted, juce::RectanglePlacement::stretchToFit);
            return;
        }

        // no picture (not captured yet, or an editor drawn with Metal/OpenGL): a simple card
        g.setColour (rack.colour.withAlpha (0.18f));
        g.fillRoundedRectangle (body, 4.0f);
        g.setColour (theme::text.withAlpha (0.9f));
        g.setFont (theme::font (juce::jlimit (11.0f, 18.0f, body.getHeight() * 0.28f), true));
        g.drawFittedText (p.getName(), body.reduced (4.0f).withTrimmedBottom (body.getHeight() * 0.35f).toNearestInt(),
                          juce::Justification::centredBottom, 1, 0.7f);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.0f));
        g.drawFittedText (p.getPluginDescription().manufacturerName,
                          body.reduced (4.0f).withTrimmedTop (body.getHeight() * 0.62f).toNearestInt(),
                          juce::Justification::centredTop, 1, 0.8f);
    }

    /** Meters view: the plugin's own meters (gain reduction, levels...) as bars. */
    void paintMeters (juce::Graphics& g, juce::Rectangle<float> body, juce::AudioPluginInstance& p)
    {
        if (showFooter)
            body.removeFromBottom (13.0f);

        shownMeters = 0;

        if (body.getHeight() < 8.0f)
            return;

        if (meters.isEmpty())
        {
            g.setColour (theme::textDim.withAlpha (0.8f));
            g.setFont (theme::font (10.5f));
            g.drawFittedText ("No meters", body.toNearestInt(), juce::Justification::centred, 2);
            return;
        }

        auto barColour = [this] (const PluginMeter& m, float v)
        {
            if (m.gainReduction)
                return theme::amber;
            return v > 0.97f ? theme::red : (v > 0.85f ? theme::amber : rack.colour);
        };

        auto value = [this] (int i) { return juce::isPositiveAndBelow (i, meterValues.size()) ? meterValues[i] : 0.0f; };

        g.setFont (theme::font (9.5f, true));

        if (body.getHeight() > body.getWidth() * 0.8f)
        {
            // tall slot: vertical bars side by side, names below
            const int n = juce::jmin (meters.size(), juce::jmax (1, (int) (body.getWidth() / 16.0f)));
            const float slotW = body.getWidth() / (float) n;
            const float barW = juce::jmin (18.0f, slotW - 4.0f);
            auto labels = body.removeFromBottom (13.0f);

            for (int i = 0; i < n; ++i)
            {
                const auto& m = meters.getReference (i);
                const auto v = value (i);
                auto bar = juce::Rectangle<float> (body.getX() + slotW * (float) i + (slotW - barW) / 2.0f, body.getY(), barW, body.getHeight());
                g.setColour (theme::meterBg);
                g.fillRoundedRectangle (bar, 2.0f);
                g.setColour (barColour (m, v));

                if (m.gainReduction) g.fillRoundedRectangle (bar.withHeight (bar.getHeight() * v), 2.0f);   // GR hangs from the top
                else                 g.fillRoundedRectangle (bar.withTrimmedTop (bar.getHeight() * (1.0f - v)), 2.0f);

                g.setColour (theme::textDim);
                g.drawFittedText (m.name, juce::Rectangle<float> (body.getX() + slotW * (float) i, labels.getY(), slotW, labels.getHeight()).toNearestInt(),
                                  juce::Justification::centred, 1, 0.6f);
            }

            shownMeters = n;
            return;
        }

        // wide slot: one row per meter (name, bar, value), in columns like the parameters
        constexpr float rowH = 15.0f, colW = 120.0f;
        const int cols = juce::jmax (1, (int) (body.getWidth() / colW));
        const int rowsFit = juce::jmax (1, (int) (body.getHeight() / rowH));
        const int n = juce::jmin (meters.size(), cols * rowsFit);
        const float colWidth = body.getWidth() / (float) cols;

        for (int i = 0; i < n; ++i)
        {
            const auto& m = meters.getReference (i);
            const auto v = value (i);
            const int c = i / rowsFit, row = i % rowsFit;
            auto r = juce::Rectangle<float> (body.getX() + (float) c * colWidth, body.getY() + (float) row * rowH,
                                             colWidth - (c < cols - 1 ? 6.0f : 0.0f), rowH - 3.0f);

            auto label = r.removeFromLeft (juce::jmin (52.0f, r.getWidth() * 0.4f));
            auto valueArea = r.removeFromRight (r.getWidth() > 90.0f ? 44.0f : 0.0f);
            g.setColour (theme::textDim);
            g.drawFittedText (m.name, label.toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);

            g.setColour (theme::meterBg);
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (barColour (m, v));

            if (m.gainReduction) g.fillRoundedRectangle (r.withLeft (r.getRight() - r.getWidth() * v), 2.0f);   // GR grows from the right
            else                 g.fillRoundedRectangle (r.withWidth (r.getWidth() * v), 2.0f);

            if (! valueArea.isEmpty())
            {
                g.setColour (theme::text.withAlpha (0.85f));
                g.drawFittedText (meterText (p, m).substring (0, 10), valueArea.toNearestInt(), juce::Justification::centredRight, 1, 0.6f);
            }
        }

        shownMeters = n;
    }

    void setView (int mode)
    {
        auto& s = rack.getSlot (index);
        s.viewMode = juce::jlimit (0, numViews - 1, mode);
        session.markDirty();
        refreshStats();
        repaint();
    }

    void toggleBypass (const juce::ModifierKeys& mods = {})
    {
        auto& s = rack.getSlot (index);
        const bool state = ! s.bypassed.load();
        s.bypassed = state;

        for (auto& [r, n] : edit.linkedSlots (rack, index, mods))
            r->getSlot (n).bypassed = state;

        repaint();
    }

    void toggleView (const juce::ModifierKeys& mods = {})
    {
        auto& s = rack.getSlot (index);
        s.viewMode = (s.viewMode + 1) % numViews;

        for (auto& [r, n] : edit.linkedSlots (rack, index, mods))
            r->getSlot (n).viewMode = s.viewMode;

        session.markDirty();
        refreshStats();
    }

    void openBrowser()
    {
        auto browser = std::make_unique<PluginBrowser> (session.getPluginManager(),
            [safe = juce::Component::SafePointer<SlotComponent> (this)] (const juce::PluginDescription& d)
            {
                if (safe == nullptr)
                    return;

                safe->session.loadPlugin (safe->rack, safe->index, d, [d] (const juce::String& error)
                {
                    if (error.isNotEmpty())
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                "Could not load " + d.name, error);
                });
            });

        // Open inside the main window: a desktop call-out closes itself whenever the app loses focus.
        auto* top = getTopLevelComponent();
        juce::CallOutBox::launchAsynchronously (std::move (browser), top->getLocalArea (this, getLocalBounds()), top);
    }

    void showMenu()
    {
        auto* p = rack.getPlugin (index);
        juce::PopupMenu m;

        // Menu actions run later: never capture `this` or the plugin raw, the UI may have
        // been rebuilt (scene recall, plugin loaded, device change) by the time one is picked.
        juce::Component::SafePointer<SlotComponent> safe (this);
        auto act = [safe] (auto fn) { return [safe, fn] { if (safe != nullptr) fn (*safe); }; };

        if (p != nullptr)
        {
            m.addItem ("Open editor", act ([] (SlotComponent& s) { s.openEditor(); }));
            m.addItem ("Plugin on", true, ! rack.getSlot (index).bypassed.load(), act ([] (SlotComponent& s) { s.toggleBypass(); }));
            {
                juce::PopupMenu views;
                const auto mode = rack.getSlot (index).viewMode;
                views.addItem ("Parameters", true, mode == 0, act ([] (SlotComponent& s) { s.setView (0); }));
                views.addItem ("Mini view", true, mode == 1, act ([] (SlotComponent& s) { s.setView (1); }));
                views.addItem ("Meters", true, mode == 2, act ([] (SlotComponent& s) { s.setView (2); }));
                m.addSubMenu ("View", views);
            }
            m.addItem ("Default slot size", rack.getSlot (index).uiSize != 1.0f, false, act ([] (SlotComponent& s)
            {
                s.rack.getSlot (s.index).uiSize = 1.0f;
                if (s.onSlotResized) s.onSlotResized();
                s.session.markDirty();
            }));
            m.addSeparator();
            m.addItem (menuItem ("Copy", "\xe2\x8c\x98" "C", act ([] (SlotComponent& s) { s.edit.copy(); })));
            m.addItem (menuItem ("Paste", "\xe2\x8c\x98V", act ([] (SlotComponent& s) { s.edit.paste(); })));
            m.addItem (menuItem ("Duplicate", "\xe2\x8c\x98" "D", act ([] (SlotComponent& s) { s.edit.duplicate(); })));
            m.addItem ("Replace...", act ([] (SlotComponent& s) { s.openBrowser(); }));
            m.addSeparator();
            m.addItem ("Latency: " + juce::String (p->getLatencySamples()) + " samples ("
                         + juce::String (1000.0 * p->getLatencySamples() / juce::jmax (1.0, rack.getSampleRate()), 1) + " ms)",
                       false, false, nullptr);

            if (const auto& sl = rack.getSlot (index); sl.hopCalls > 0)
                m.addItem ("Internal hop: " + juce::String (sl.hopCalls * rack.getBlockSize()) + " samples, spread to phase "
                             + juce::String (sl.hopPhase + 1) + "/" + juce::String (sl.hopCalls),
                           false, false, nullptr);

            juce::PopupMenu params;
            const auto& all = p->getParameters();
            const auto shown = rack.getSlot (index).shownParams;

            for (int i = 0; i < all.size() && i < 128; ++i)
                params.addItem (juce::PopupMenu::Item (all[i]->getName (48).isNotEmpty() ? all[i]->getName (48) : "Parameter " + juce::String (i + 1))
                                    .setTicked (shown.contains (i))
                                    .setAction (act ([i] (SlotComponent& s)
                                    {
                                        auto& sl = s.rack.getSlot (s.index);
                                        sl.paramsChosen = true;

                                        if (sl.shownParams.contains (i)) sl.shownParams.removeFirstMatchingValue (i);
                                        else                             sl.shownParams.add (i);

                                        s.syncRows();
                                        s.session.markDirty();
                                    })));

            params.addSeparator();
            params.addItem ("Default selection", act ([] (SlotComponent& s)
            {
                s.rack.getSlot (s.index).paramsChosen = false;
                s.syncRows();
                s.session.markDirty();
            }));
            params.addItem ("Hide all", act ([] (SlotComponent& s)
            {
                auto& sl = s.rack.getSlot (s.index);
                sl.paramsChosen = true;
                sl.shownParams.clearQuick();
                s.syncRows();
                s.session.markDirty();
            }));
            m.addSubMenu ("Parameters on rack", params);
            m.addSeparator();

            m.addItem (menuItem ("Remove", "\xe2\x8c\xab", act ([] (SlotComponent& s) { s.session.removePlugin (s.rack, s.index); })));
        }
        else
        {
            m.addItem ("Insert plugin...", act ([] (SlotComponent& s) { s.openBrowser(); }));
            m.addItem (menuItem ("Paste", "\xe2\x8c\x98V", act ([] (SlotComponent& s) { s.edit.paste(); })));
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
    }

    void openEditor()
    {
        if (auto* p = rack.getPlugin (index))
            PluginWindow::show (*p, rack.name + "  ·  " + p->getName());
    }

    Session& session;
    EditController& edit;
    Rack& rack;
    const int index;
    juce::Rectangle<float> powerArea, viewArea;
    bool dragOver = false, shownBypass = false, shownSelected = false, showFooter = true, vertical = false;
    int shownCpu = -2, shownView = -1, shownThumb = -1, hiddenRows = 0, shownMeters = 0, shownLatency = -1;
    static constexpr int numViews = 3;
    juce::Array<PluginMeter> meters;       // the plugin's own meters (see PluginMeters)
    juce::Array<float> meterValues;        // last painted values
    const juce::AudioPluginInstance* meterPlugin = nullptr;
    juce::OwnedArray<ParamRow> rows;
    const juce::AudioPluginInstance* shownPlugin = nullptr;
    SlotResizer resizer { *this };
};

//==============================================================================
/** Thin handle on the rack's bottom edge (horizontal) or right edge (vertical). */
class RackComponent::ResizeHandle final : public juce::Component
{
public:
    explicit ResizeHandle (RackComponent& o) : owner (o) {}

    void mouseEnter (const juce::MouseEvent&) override
    {
        setMouseCursor (owner.vertical ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        startSize = owner.rack.uiSize;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto base = (float) (owner.vertical ? baseWidth : baseHeight);
        const auto delta = (float) (owner.vertical ? e.getDistanceFromDragStartX() : e.getDistanceFromDragStartY());
        owner.rack.uiSize = juce::jlimit (EditController::minRackSize, EditController::maxRackSize, startSize + delta / base);

        if (owner.onResized)
            owner.onResized();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        owner.session.markDirty();
    }

    void paint (juce::Graphics& g) override
    {
        if (isMouseOver() || isMouseButtonDown())
        {
            g.setColour (theme::accent.withAlpha (0.6f));
            g.fillRect (getLocalBounds().reduced (owner.vertical ? 1 : 30, owner.vertical ? 30 : 1));
        }
    }

private:
    RackComponent& owner;
    float startSize = 1.0f;
};

//==============================================================================
RackComponent::RackComponent (Session& s, EditController& e, Rack& r, int i)
    : session (s), edit (e), rack (r), index (i)
{
    nameLabel.setText (rack.name, juce::dontSendNotification);
    nameLabel.setFont (theme::font (14.0f, true));
    nameLabel.setEditable (false, true, false);
    nameLabel.onTextChange = [this] { rack.name = nameLabel.getText(); session.markDirty(); };
    addAndMakeVisible (nameLabel);

    // clicks, drags and right-clicks on the name act on the rack (double-click still renames)
    nameLabel.addMouseListener (this, false);

    for (auto* box : { &inputBox, &outputBox })
    {
        box->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (*box);
    }

    // Alt while picking = same routing on all selected racks;
    // Alt+Shift (or Alt+Cmd) = consecutive channels from the picked one, starting at this rack.
    inputBox.onChange = [this]
    {
        const auto id = inputBox.getSelectedId();
        const auto mods = juce::ModifierKeys::getCurrentModifiers();
        const auto linked = edit.linkedRacks (rack, mods);
        auto& engine = session.getEngine();

        if (id >= 3000)
        {
            // chained: this rack is fed by another rack's output (same callback, zero latency)
            const bool ok = engine.setRackInputSource (rack, id - 3000);

            for (auto* r : linked)
                engine.setRackInputSource (*r, id - 3000);   // refuses the source itself and loops

            if (! ok)
                fillRoutingBoxes();

            session.markDirty();
            return;
        }

        const int ch = id == 1 ? -1 : (id >= 2000 ? id - 2000 : id - 1000);
        const bool stereo = id >= 1000 && id < 2000;

        if (! linked.isEmpty() && ch >= 0 && (mods.isShiftDown() || mods.isCommandDown()))
        {
            session.setConsecutiveInputs (edit.targetRacks(), stereo, ch, rack.getId());
            return;
        }

        juce::Array<Rack*> all (linked);
        all.add (&rack);

        for (auto* r : all)
        {
            engine.setRackInputSource (*r, -1);
            r->inputChannel = ch;
            r->stereoInput = stereo;
        }

        session.markDirty();
    };

    outputBox.onChange = [this]
    {
        const auto id = outputBox.getSelectedId();
        const auto mods = juce::ModifierKeys::getCurrentModifiers();
        const auto linked = edit.linkedRacks (rack, mods);
        const int ch = id == 1 ? -1 : (id >= 2000 ? id - 2000 : id - 1000);
        const bool stereo = id < 2000;

        if (! linked.isEmpty() && ch >= 0 && (mods.isShiftDown() || mods.isCommandDown()))
        {
            session.setConsecutiveOutputs (edit.targetRacks(), stereo, ch, rack.getId());
            return;
        }

        juce::Array<Rack*> all (linked);
        all.add (&rack);

        for (auto* r : all)
        {
            r->outputChannel = ch;
            r->stereoOutput = stereo;
        }

        session.markDirty();
    };

    for (int n = 0; n < Rack::maxSlots; ++n)
    {
        auto* slot = slots.add (new SlotComponent (session, edit, rack, n));
        slot->onSlotResized = [this] { resized(); };
        addAndMakeVisible (slot);
    }

    gain.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    gain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 14);
    gain.setRange (-60.0, 12.0, 0.1);
    gain.setSkewFactorFromMidPoint (-12.0);
    gain.setDoubleClickReturnValue (true, 0.0, {});                 // no Alt-click reset: Alt = all selected racks
    gain.setVelocityModeParameters (1.0, 1, 0.0, false);            // no modifier swaps the drag mode
    gain.setTextValueSuffix (" dB");
    gain.setColour (juce::Slider::rotarySliderFillColourId, rack.colour);
    gain.setValue (rack.gainDb.load(), juce::dontSendNotification);
    // Alt = same gain on all selected racks, Alt+Shift = relative (keeps their differences).
    gain.onDragStart = [this] { gainDragMods = juce::ModifierKeys::getCurrentModifiers(); gainDragging = true; };
    gain.onValueChange = [this]
    {
        const auto v = (float) gain.getValue();
        const auto delta = v - rack.gainDb.load();
        const auto mods = gainDragging ? gainDragMods : juce::ModifierKeys::getCurrentModifiers();
        rack.gainDb = v;

        for (auto* r : edit.linkedRacks (rack, mods))
            r->gainDb = mods.isShiftDown() ? juce::jlimit (-60.0f, 12.0f, r->gainDb.load() + delta) : v;
    };
    gain.onDragEnd = [this] { gainDragging = false; session.markDirty(); };
    addAndMakeVisible (gain);

    auto setupToggle = [this] (juce::TextButton& b, juce::Colour on, std::atomic<bool> Rack::* flag)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonOnColourId, on);
        b.setToggleState ((rack.*flag).load(), juce::dontSendNotification);
        b.onClick = [this, &b, flag]
        {
            const bool state = b.getToggleState();
            rack.*flag = state;

            // Alt = same state on all selected racks
            for (auto* r : edit.linkedRacks (rack, juce::ModifierKeys::getCurrentModifiers()))
                r->*flag = state;

            session.markDirty();
            repaint();
        };
        addAndMakeVisible (b);
    };

    setupToggle (muteButton, theme::red, &Rack::muted);
    setupToggle (soloButton, theme::amber, &Rack::solo);
    setupToggle (bypassButton, theme::accent, &Rack::bypassed);
    muteButton.setTooltip ("Mute rack  (Alt: all selected racks)");
    soloButton.setTooltip ("Solo rack  (Alt: all selected racks)");
    bypassButton.setTooltip ("Bypass whole rack  (Alt: all selected racks)");
    gain.setTooltip ("Rack gain  (Alt: same on all selected racks, Alt+Shift: relative)");
    inputBox.setTooltip ("Input  (Alt: same on all selected, Alt+Shift: consecutive from here)");
    outputBox.setTooltip ("Output  (Alt: same on all selected, Alt+Shift: consecutive from here)");

    infoLabel.setFont (theme::font (10.5f));
    infoLabel.setColour (juce::Label::textColourId, theme::textDim);
    infoLabel.setJustificationType (juce::Justification::topLeft);
    infoLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (infoLabel);

    statusLabel.setFont (theme::font (10.5f, true));
    statusLabel.setColour (juce::Label::textColourId, theme::amber);
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (statusLabel);

    menuButton.setTooltip ("Rack options (also: right-click anywhere on the rack)");
    menuButton.onClick = [this]
    {
        if (! edit.isRackSelected (rack.getId()))
            edit.clickRack (rack.getId(), {});
        showRackMenu();
    };
    addAndMakeVisible (menuButton);

    resizeHandle = std::make_unique<ResizeHandle> (*this);
    addAndMakeVisible (*resizeHandle);

    refresh();
}

void RackComponent::lookAndFeelChanged()
{
    muteButton.setColour (juce::TextButton::buttonOnColourId, theme::red);
    soloButton.setColour (juce::TextButton::buttonOnColourId, theme::amber);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, theme::accent);
    infoLabel.setColour (juce::Label::textColourId, theme::textDim);
}

RackComponent::~RackComponent()
{
    nameLabel.removeMouseListener (this);
}

int RackComponent::getPreferredLength() const
{
    return juce::roundToInt ((float) (vertical ? baseWidth : baseHeight) * rack.uiSize);
}

void RackComponent::fillRoutingBoxes()
{
    inputBox.clear (juce::dontSendNotification);
    outputBox.clear (juce::dontSendNotification);

    juce::StringArray ins, outs;

    if (auto* dev = session.getEngine().getDeviceManager().getCurrentAudioDevice())
    {
        const auto activeIns = dev->getActiveInputChannels();
        const auto activeOuts = dev->getActiveOutputChannels();
        ins = dev->getInputChannelNames();
        outs = dev->getOutputChannelNames();

        // only channels that are enabled in the device setup
        for (int c = ins.size(); --c >= 0;)
            if (! activeIns[c]) ins.set (c, {});
        for (int c = outs.size(); --c >= 0;)
            if (! activeOuts[c]) outs.set (c, {});
    }

    inputBox.addItem ("IN: none", 1);
    for (int c = 0; c + 1 < ins.size(); c += 2)
        if (ins[c].isNotEmpty() && ins[c + 1].isNotEmpty())
            inputBox.addItem ("IN " + juce::String (c + 1) + "-" + juce::String (c + 2) + " st", 1000 + c);
    for (int c = 0; c < ins.size(); ++c)
        if (ins[c].isNotEmpty())
            inputBox.addItem ("IN " + juce::String (c + 1) + " mono", 2000 + c);

    outputBox.addItem ("OUT: none", 1);
    for (int c = 0; c + 1 < outs.size(); c += 2)
        if (outs[c].isNotEmpty() && outs[c + 1].isNotEmpty())
            outputBox.addItem ("OUT " + juce::String (c + 1) + "-" + juce::String (c + 2) + " st", 1000 + c);
    for (int c = 0; c < outs.size(); ++c)
        if (outs[c].isNotEmpty())
            outputBox.addItem ("OUT " + juce::String (c + 1) + " mono", 2000 + c);

    // other racks as sources (chaining); those that would create a loop are disabled
    auto& engine = session.getEngine();
    bool anyRack = false;

    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        auto& other = engine.getRack (i);

        if (&other == &rack)
            continue;

        if (! anyRack)
        {
            inputBox.addSectionHeading ("From rack");
            anyRack = true;
        }

        inputBox.addItem (juce::String::fromUTF8 ("\xe2\x97\x80 ") + juce::String (i + 1).paddedLeft ('0', 2) + "  " + other.name,
                          3000 + other.getId());
        inputBox.setItemEnabled (3000 + other.getId(), ! engine.wouldCreateCycle (rack, other.getId()));
    }

    const auto in = rack.inputChannel.load();

    if (const auto src = rack.inputRackId.load(); src >= 0 && engine.findRack (src) != nullptr)
        inputBox.setSelectedId (3000 + src, juce::dontSendNotification);
    else
        inputBox.setSelectedId (in < 0 ? 1 : (rack.stereoInput.load() ? 1000 + in : 2000 + in), juce::dontSendNotification);

    if (inputBox.getSelectedId() == 0 && rack.inputRackId < 0)
        inputBox.setText ("IN " + juce::String (in + 1) + (rack.stereoInput.load() ? " st" : " mono") + " (off)", juce::dontSendNotification);

    const auto out = rack.outputChannel.load();
    outputBox.setSelectedId (out < 0 ? 1 : (rack.stereoOutput.load() ? 1000 + out : 2000 + out), juce::dontSendNotification);

    if (outputBox.getSelectedId() == 0)
        outputBox.setText ("OUT " + juce::String (out + 1) + (rack.stereoOutput.load() ? " st" : " mono") + " (off)", juce::dontSendNotification);
}

void RackComponent::refresh()
{
    nameLabel.setText (rack.name, juce::dontSendNotification);
    gain.setValue (rack.gainDb.load(), juce::dontSendNotification);
    muteButton.setToggleState (rack.muted.load(), juce::dontSendNotification);
    soloButton.setToggleState (rack.solo.load(), juce::dontSendNotification);
    bypassButton.setToggleState (rack.bypassed.load(), juce::dontSendNotification);
    gain.setColour (juce::Slider::rotarySliderFillColourId, rack.colour);
    fillRoutingBoxes();

    for (auto* s : slots)
        s->repaint();

    repaint();
}

void RackComponent::updateMeters()
{
    for (int c = 0; c < 2; ++c)
    {
        inLevel[c]  = juce::jmax (rack.inMeter[c].readAndReset(),  inLevel[c] * 0.82f);
        outLevel[c] = juce::jmax (rack.outMeter[c].readAndReset(), outLevel[c] * 0.82f);
    }

    const auto sr = juce::jmax (1.0, rack.getSampleRate());
    const auto latMs = 1000.0 * rack.getLatencySamples() / sr;
    peakLoad = juce::jmax (rack.peakLoad.readAndReset(), peakLoad * 0.97f);

    const auto totalMs = 1000.0 * rack.totalLatencySamples.load() / sr;
    const auto group = rack.latencyGroup.load();

    juce::String text;
    text << "core " << (rack.lastThread.load() + 1)
         << "\n" << juce::roundToInt (100.0f * rack.cpuLoad.load()) << "%  pk " << juce::roundToInt (100.0f * peakLoad) << "%"
         << "\nlat " << juce::String (juce::jmax (latMs, totalMs), 1) << " ms";

    infoLabel.setText (text, juce::dontSendNotification);
    infoLabel.setColour (juce::Label::textColourId, peakLoad > 0.9f ? theme::amber : theme::textDim);

    if (peakLoad > 0.9f)
    {
        statusLabel.setColour (juce::Label::textColourId, theme::amber);
        statusLabel.setText (juce::String::fromUTF8 ("\xe2\x9a\xa0 CPU spikes"), juce::dontSendNotification);
    }
    else if (group > 0)
    {
        const auto compMs = 1000.0 * rack.compensationSamples.load() / sr;
        statusLabel.setColour (juce::Label::textColourId, theme::accent);
        statusLabel.setText ("LG " + juce::String::charToString ((juce::juce_wchar) ('A' + group - 1))
                               + (compMs > 0.05 ? "  +" + juce::String (compMs, 1) + " ms" : juce::String()),
                             juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText ({}, juce::dontSendNotification);
    }

    repaint (meterArea);

    // values changed from elsewhere (Alt on another rack, MIDI, scenes) show up here
    if (! gain.isMouseButtonDown() && std::abs ((float) gain.getValue() - rack.gainDb.load()) > 0.001f)
        gain.setValue (rack.gainDb.load(), juce::dontSendNotification);

    muteButton.setToggleState (rack.muted.load(), juce::dontSendNotification);
    soloButton.setToggleState (rack.solo.load(), juce::dontSendNotification);
    bypassButton.setToggleState (rack.bypassed.load(), juce::dontSendNotification);

    for (auto* s : slots)
        if (s->isShowing())
            s->refreshStats();
}

void RackComponent::setVertical (bool shouldBeVertical)
{
    vertical = shouldBeVertical;
    infoLabel.setJustificationType (vertical ? juce::Justification::centred : juce::Justification::topLeft);

    for (auto* s : slots)
        s->setVertical (vertical);

    resized();
    repaint();
}

void RackComponent::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (vertical ? 2.0f : 0.0f, vertical ? 0.0f : 2.0f);
    const bool active = ! rack.muted.load() && ! rack.bypassed.load();
    const bool selected = edit.isRackSelected (rack.getId());

    g.setColour (selected ? theme::panelLight : theme::panel);
    g.fillRoundedRectangle (b, 6.0f);

    if (selected)
    {
        g.setColour (theme::accent);
        g.drawRoundedRectangle (b.reduced (1.0f), 6.0f, 2.0f);
    }

    g.setColour (active ? rack.colour : rack.colour.withSaturation (0.1f).darker (0.4f));
    g.fillRoundedRectangle (vertical ? b.removeFromTop (6.0f) : b.removeFromLeft (6.0f), 3.0f);

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f, true));
    g.drawText (juce::String (index + 1).paddedLeft ('0', 2), indexArea, juce::Justification::centredLeft);

    // meters: in L/R, out L/R
    auto m = meterArea.toFloat();
    const float w = (m.getWidth() - 6) / 4.0f;
    theme::drawMeter (g, { m.getX(),                 m.getY(), w - 1, m.getHeight() }, inLevel[0]);
    theme::drawMeter (g, { m.getX() + w,             m.getY(), w - 1, m.getHeight() }, inLevel[1]);
    theme::drawMeter (g, { m.getX() + 2 * w + 6,     m.getY(), w - 1, m.getHeight() }, outLevel[0]);
    theme::drawMeter (g, { m.getX() + 3 * w + 6,     m.getY(), w - 1, m.getHeight() }, outLevel[1]);
}

void RackComponent::paintOverChildren (juce::Graphics& g)
{
    if (dropIndicator == 0)
        return;

    g.setColour (theme::accent);

    if (vertical)
        g.fillRect (dropIndicator < 0 ? 0 : getWidth() - 3, 0, 3, getHeight());
    else
        g.fillRect (0, dropIndicator < 0 ? 0 : getHeight() - 3, getWidth(), 3);
}

void RackComponent::resized()
{
    if (vertical)
    {
        resizeHandle->setBounds (getWidth() - 5, 0, 5, getHeight());
        auto r = getLocalBounds().reduced (8, 6).withTrimmedTop (8).withTrimmedRight (2);

        auto nameRow = r.removeFromTop (20);
        indexArea = nameRow.removeFromLeft (22);
        menuButton.setBounds (nameRow.removeFromRight (28));
        nameLabel.setBounds (nameRow);
        statusLabel.setBounds (r.removeFromTop (16));
        inputBox.setBounds (r.removeFromTop (21));
        r.removeFromTop (3);
        outputBox.setBounds (r.removeFromTop (21));
        r.removeFromTop (6);

        infoLabel.setBounds (r.removeFromBottom (42));

        auto controls = r.removeFromBottom (78);
        r.removeFromBottom (6);
        auto buttons = controls.removeFromRight (28);
        const int bh = (buttons.getHeight() - 4) / 3;
        muteButton.setBounds (buttons.removeFromTop (bh));
        buttons.removeFromTop (2);
        soloButton.setBounds (buttons.removeFromTop (bh));
        buttons.removeFromTop (2);
        bypassButton.setBounds (buttons.removeFromTop (bh));
        controls.removeFromRight (4);
        meterArea = controls.removeFromLeft (26).reduced (0, 2);
        controls.removeFromLeft (4);
        gain.setBounds (controls);

        layoutSlots (r);
        return;
    }

    resizeHandle->setBounds (0, getHeight() - 5, getWidth(), 5);

    const bool compact = getHeight() < 72;   // shrunk rack: name + IN/OUT on two rows
    auto r = getLocalBounds().reduced (6, compact ? 4 : 6).withTrimmedLeft (8).withTrimmedBottom (2);

    auto header = r.removeFromLeft (290);
    auto nameRow = header.removeFromTop (compact ? 18 : 20);
    indexArea = nameRow.removeFromLeft (22);
    menuButton.setBounds (nameRow.removeFromRight (28));
    nameRow.removeFromRight (4);
    statusLabel.setBounds (nameRow.removeFromRight (96));
    nameLabel.setBounds (nameRow);
    header.removeFromTop (3);

    if (compact)
    {
        auto row = header.removeFromTop (juce::jmin (21, header.getHeight()));
        inputBox.setBounds (row.removeFromLeft (row.getWidth() / 2 - 2));
        row.removeFromLeft (4);
        outputBox.setBounds (row);
    }
    else
    {
        inputBox.setBounds (header.removeFromTop (21));
        header.removeFromTop (3);
        outputBox.setBounds (header.removeFromTop (21));
    }

    r.removeFromLeft (8);
    infoLabel.setBounds (r.removeFromRight (104).reduced (4, 0));

    auto buttons = r.removeFromRight (28);
    const int bh = (buttons.getHeight() - 4) / 3;
    muteButton.setBounds (buttons.removeFromTop (bh));
    buttons.removeFromTop (2);
    soloButton.setBounds (buttons.removeFromTop (bh));
    buttons.removeFromTop (2);
    bypassButton.setBounds (buttons.removeFromTop (bh));

    r.removeFromRight (4);
    gain.setBounds (r.removeFromRight (58));
    r.removeFromRight (4);
    meterArea = r.removeFromRight (26).reduced (0, 2);
    r.removeFromRight (8);

    layoutSlots (r);
}

void RackComponent::layoutSlots (juce::Rectangle<int> area)
{
    // Every slot wants (base length x its size factor). When that doesn't fit, empty slots
    // shrink first (down to a minimum), then the plugin slots scale down proportionally.
    const auto length = (float) (vertical ? area.getHeight() : area.getWidth());
    const auto base = length / (float) Rack::maxSlots;
    const auto minEmpty = juce::jmin (base, 26.0f);

    float pluginWant = 0.0f, emptyWant = 0.0f;
    int numEmpty = 0;

    for (int i = 0; i < slots.size(); ++i)
    {
        const auto want = base * rack.getSlot (i).uiSize;

        if (rack.getPlugin (i) != nullptr) pluginWant += want;
        else                              { emptyWant += want; ++numEmpty; }
    }

    float pluginScale = 1.0f, emptyScale = 1.0f;

    if (pluginWant + emptyWant > length)
    {
        const auto emptyRoom = juce::jmax ((float) numEmpty * minEmpty, length - pluginWant);
        emptyScale = emptyWant > 0.0f ? juce::jmin (1.0f, emptyRoom / emptyWant) : 1.0f;
        pluginScale = pluginWant > 0.0f ? juce::jmin (1.0f, (length - emptyWant * emptyScale) / pluginWant) : 1.0f;
    }

    // whatever is left over (or missing after rounding) goes to the empty slots, else spread
    std::vector<float> lens ((size_t) slots.size());
    float used = 0.0f;

    for (int i = 0; i < slots.size(); ++i)
    {
        lens[(size_t) i] = base * rack.getSlot (i).uiSize * (rack.getPlugin (i) != nullptr ? pluginScale : emptyScale);
        used += lens[(size_t) i];
    }

    const auto spare = length - used;

    for (int i = 0; i < slots.size(); ++i)
        if (numEmpty > 0 ? rack.getPlugin (i) == nullptr : true)
            lens[(size_t) i] += spare / (float) (numEmpty > 0 ? numEmpty : slots.size());

    float pos = 0.0f;

    for (int i = 0; i < slots.size(); ++i)
    {
        const auto start = juce::roundToInt (pos);
        pos += lens[(size_t) i];
        const auto end = i == slots.size() - 1 ? (int) length : juce::roundToInt (pos);

        if (vertical)
            slots[i]->setBounds (area.getX(), area.getY() + start, area.getWidth(), end - start);
        else
            slots[i]->setBounds (area.getX() + start, area.getY(), end - start, area.getHeight());
    }
}

//==============================================================================
void RackComponent::mouseDown (const juce::MouseEvent& e)
{
    // Also receives events from the name label (see the constructor).
    if (e.mods.isPopupMenu())
    {
        if (! edit.isRackSelected (rack.getId()))
            edit.clickRack (rack.getId(), {});

        showRackMenu();
        return;
    }

    edit.clickRack (rack.getId(), e.mods);
}

void RackComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || e.getDistanceFromDragStart() < 8)
        return;

    if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor (this))
        if (! dnd->isDragAndDropActive())
            dnd->startDragging ("rack:" + juce::String (rack.getId()), this);
}

bool RackComponent::dropsAfter (juce::Point<int> p) const
{
    return vertical ? p.x > getWidth() / 2 : p.y > getHeight() / 2;
}

bool RackComponent::isInterestedInDragSource (const SourceDetails& d)
{
    return d.description.toString().startsWith ("rack:");
}

void RackComponent::itemDragMove (const SourceDetails& d)
{
    const auto newIndicator = dropsAfter (d.localPosition) ? 1 : -1;

    if (newIndicator != dropIndicator)
    {
        dropIndicator = newIndicator;
        repaint();
    }
}

void RackComponent::itemDragExit (const SourceDetails&)
{
    dropIndicator = 0;
    repaint();
}

void RackComponent::itemDropped (const SourceDetails& d)
{
    dropIndicator = 0;
    repaint();

    // move the rack before/after this one; with Alt held, drop a copy there
    const auto rackId = d.description.toString().fromFirstOccurrenceOf ("rack:", false, false).getIntValue();
    const auto target = session.getEngine().indexOf (rack) + (dropsAfter (d.localPosition) ? 1 : 0);
    edit.dropRack (rackId, target, altDown());
}

void RackComponent::showRackMenu()
{
    juce::Component::SafePointer<RackComponent> safe (this);
    auto act = [safe] (auto fn) { return [safe, fn] { if (safe != nullptr) fn (*safe); }; };
    auto& e = edit;

    juce::PopupMenu m;
    m.addItem ("Rename", act ([] (RackComponent& rc) { rc.nameLabel.showEditor(); }));
    m.addItem (menuItem ("Copy", "\xe2\x8c\x98" "C", [&e] { e.copy(); }));
    m.addItem (menuItem ("Paste", "\xe2\x8c\x98V", [&e] { e.paste(); }));
    m.addItem (menuItem ("Duplicate", "\xe2\x8c\x98" "D", [&e] { e.duplicate(); }));
    m.addSeparator();

    juce::PopupMenu io;
    io.addItem (menuItem ("Consecutive inputs, mono", "\xe2\x8c\x98\xe2\x8c\xa5I", [&e] { e.consecutiveInputs (false); }));
    io.addItem (menuItem ("Consecutive inputs, stereo", "\xe2\x8c\x98\xe2\x87\xa7I", [&e] { e.consecutiveInputs (true); }));
    io.addItem (menuItem ("Consecutive outputs, mono", "\xe2\x8c\x98\xe2\x8c\xa5O", [&e] { e.consecutiveOutputs (false); }));
    io.addItem (menuItem ("Consecutive outputs, stereo", "\xe2\x8c\x98\xe2\x87\xa7O", [&e] { e.consecutiveOutputs (true); }));
    m.addSubMenu ("Selected racks I/O", io);

    juce::PopupMenu size;
    size.addItem (menuItem ("Larger", "\xe2\x8c\x98=", [&e] { e.resizeTargets (1.15f); }));
    size.addItem (menuItem ("Smaller", "\xe2\x8c\x98-", [&e] { e.resizeTargets (1.0f / 1.15f); }));
    size.addItem ("Default size", act ([] (RackComponent& rc)
    {
        rc.rack.uiSize = 1.0f;
        if (rc.onResized) rc.onResized();
        rc.session.markDirty();
    }));
    m.addSubMenu ("Size", size);

    juce::PopupMenu groups;
    for (int g = 0; g <= 8; ++g)
        groups.addItem (juce::PopupMenu::Item (g == 0 ? juce::String ("None") : "Group " + juce::String::charToString ((juce::juce_wchar) ('A' + g - 1)))
                            .setTicked (rack.latencyGroup.load() == g)
                            .setAction (act ([g] (RackComponent& rc)
                            {
                                for (auto id : rc.edit.targetRacks())
                                    if (auto* r = rc.session.getEngine().findRack (id))
                                        r->latencyGroup = g;

                                rc.session.getEngine().updateLatencyCompensation();
                                rc.session.markDirty();
                            })));
    m.addSubMenu ("Latency group", groups);

    juce::PopupMenu colours;
    const juce::Colour palette[] = { juce::Colour (0xff31c4f3), juce::Colour (0xff7ed957), juce::Colour (0xffffb341),
                                     juce::Colour (0xffff5f6d), juce::Colour (0xffb98cff), juce::Colour (0xff45e0c0),
                                     juce::Colour (0xfff26fd1), juce::Colour (0xffe8e35a) };
    for (auto c : palette)
        colours.addItem (juce::PopupMenu::Item (juce::String::repeatedString (juce::String::charToString (0x25A0), 5))
                             .setColour (c)
                             .setTicked (rack.colour == c)
                             .setAction (act ([c] (RackComponent& rc)
                             {
                                 for (auto id : rc.edit.targetRacks())
                                     if (auto* r = rc.session.getEngine().findRack (id))
                                         r->colour = c;
                                 rc.session.markDirty();
                             })));

    m.addSubMenu ("Colour", colours);
    m.addSeparator();
    m.addItem (menuItem ("Delete", "\xe2\x8c\xab", [&e] { e.deleteSelection(); }));

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton));
}

} // namespace stagerack
