#include "RackComponent.h"
#include "RackViewMenu.h"
#include "AutomixerWindow.h"
#include "TempoWindow.h"
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
        // slots that don't all fit scroll: then the wheel (two fingers) scrolls, it doesn't edit
        if (auto* vp = findParentComponentOfClass<juce::Viewport>(); vp != nullptr && (vp->canScrollVertically() || vp->canScrollHorizontally()))
        {
            juce::Component::mouseWheelMove (e, w);
            return;
        }

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

        const auto slotColour = theme::tinted (theme::slot, rack.colour, 0.09f);
        g.setColour (dragOver ? theme::accent.withAlpha (0.25f)
                              : (isMouseOver (true) ? theme::tinted (theme::slotHover, rack.colour, 0.09f) : slotColour));
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
        const bool narrowHeader = header.getWidth() < 90;

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
        g.setFont (theme::font (narrowHeader ? 11.0f : 12.0f, true));
        auto nameArea = header.withTrimmedLeft (21.0f).withTrimmedRight (viewArea.isEmpty() ? 0.0f : 20.0f);
        g.drawFittedText (p->getName(), nameArea.toNearestInt(), juce::Justification::centredLeft, 1, narrowHeader ? 0.55f : 0.7f);

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
            g.setColour (slotColour.withAlpha (0.55f));
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

        // Alt or Shift on a slot of a selected rack: the same change on all selected racks
        // (power, view, insert on an empty slot, the right-click menu). The rack selection is kept,
        // so it can be repeated. An active edit group links the same changes without modifiers.
        if ((e.mods.isAltDown() || e.mods.isShiftDown()) && edit.isRackSelected (rack.getId()))
        {
            const auto mods = e.mods.withoutMouseButtons();

            if (e.mods.isPopupMenu()) showMenu (mods);
            else if (p == nullptr)    openBrowser (mods);
            else if (onPower)         toggleBypass (mods);
            else if (onView)          toggleView (mods);
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

        for (auto& [r, n] : edit.linkedSlots (rack, index, mods, EditController::Link::plugin))
            r->getSlot (n).bypassed = state;

        session.markDirty();   // saved with the session, and an undo step
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

    /** Insert from the browser into this slot, and (Alt/Shift on a selected rack, or an active edit
        group) into the other linked racks: same slot number if free, else their first free slot. */
    void openBrowser (const juce::ModifierKeys& mods = {}, bool linked = true)
    {
        juce::Array<int> others;

        if (linked && rack.getPlugin (index) == nullptr)
            for (auto* r : edit.linkedRacks (rack, mods, EditController::Link::plugin))
                others.add (r->getId());

        auto browser = std::make_unique<PluginBrowser> (session.getPluginManager(),
            [safe = juce::Component::SafePointer<SlotComponent> (this), others] (const juce::PluginDescription& d)
            {
                if (safe == nullptr)
                    return;

                auto& session = safe->session;
                auto report = [d] (const juce::String& error)
                {
                    if (error.isNotEmpty())
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                "Could not load " + d.name, error);
                };

                const auto slotIndex = safe->index;
                auto oneStep = std::make_shared<bool> (false);
                session.loadPlugin (safe->rack, slotIndex, d, report, oneStep);

                for (auto id : others)
                {
                    auto* r = session.getEngine().findRack (id);

                    if (r == nullptr)
                        continue;

                    int target = r->getPlugin (slotIndex) == nullptr ? slotIndex : -1;

                    for (int n = 0; n < Rack::maxSlots && target < 0; ++n)
                        if (r->getPlugin (n) == nullptr)
                            target = n;

                    if (target >= 0)   // a full rack is skipped: nothing is ever replaced this way
                        session.loadPlugin (*r, target, d, report, oneStep);
                }
            });

        // Open inside the main window: a desktop call-out closes itself whenever the app loses focus.
        auto* top = getTopLevelComponent();
        juce::CallOutBox::launchAsynchronously (std::move (browser), top->getLocalArea (this, getLocalBounds()), top);
    }

    void showMenu (const juce::ModifierKeys mods = {})
    {
        auto* p = rack.getPlugin (index);
        juce::PopupMenu m;
        using Link = EditController::Link;

        // how many other racks the plugin commands reach (Alt/Shift selection or edit group)
        const auto numLinked = p != nullptr ? (int) edit.linkedSlots (rack, index, mods, Link::plugin).size()
                                            : edit.linkedRacks (rack, mods, Link::plugin).size();
        const auto onAll = numLinked > 0 ? "  (" + juce::String (numLinked + 1) + " racks)" : juce::String();

        // Menu actions run later: never capture `this` or the plugin raw, the UI may have
        // been rebuilt (scene recall, plugin loaded, device change) by the time one is picked.
        juce::Component::SafePointer<SlotComponent> safe (this);
        auto act = [safe] (auto fn) { return [safe, fn] { if (safe != nullptr) fn (*safe); }; };

        if (p != nullptr)
        {
            m.addItem ("Open editor", act ([] (SlotComponent& s) { s.openEditor(); }));
            m.addItem ("Plugin on" + onAll, true, ! rack.getSlot (index).bypassed.load(), act ([mods] (SlotComponent& s) { s.toggleBypass (mods); }));
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
            m.addItem ("Replace...", act ([] (SlotComponent& s) { s.openBrowser ({}, false); }));
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

            m.addItem (menuItem ("Remove" + onAll, "\xe2\x8c\xab", act ([mods] (SlotComponent& s)
            {
                auto where = s.edit.linkedSlots (s.rack, s.index, mods, Link::plugin);
                where.insert (where.begin(), { &s.rack, s.index });
                s.session.removePlugins (where);   // may rebuild this component: nothing after this
            })));
        }
        else
        {
            m.addItem ("Insert plugin..." + onAll, act ([mods] (SlotComponent& s) { s.openBrowser (mods); }));
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
/** Divider inside one rack: between the plugins and the sends (Sends) or between the input
    section and the plugins (Input). Drag = this rack only, all the way (only sends / only
    plugins), double-click = automatic. */
class RackComponent::SplitHandle final : public juce::Component,
                                         public juce::SettableTooltipClient
{
public:
    enum Kind { Sends, Input, Output };

    SplitHandle (RackComponent& o, Kind k) : owner (o), kind (k)
    {
        setTooltip (kind == Sends  ? "Drag: plugins / sends room (all the way = only sends or only plugins)  -  double-click: automatic"
                  : kind == Input  ? "Drag: room for the input meter and trim (taller = big meter + fader)  -  double-click: automatic"
                                   : "Drag: room for the output meter and gain (taller = big meter + fader)  -  double-click: automatic");
    }

    void mouseEnter (const juce::MouseEvent&) override
    {
        setMouseCursor (owner.vertical || kind != Sends ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::LeftRightResizeCursor);
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (kind == Input)
            startLength = (float) owner.inputLength;
        else if (kind == Output)
            startLength = (float) owner.outputLength;
        else
            startLength = owner.sendsView.isVisible() ? (float) (owner.vertical ? owner.sendsView.getHeight() : owner.sendsView.getWidth()) : 0.0f;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (kind == Input)
        {
            // the input section sits above: dragging down gives it more room
            owner.rack.uiInputV = juce::jmax (0.0f, startLength + (float) e.getDistanceFromDragStartY());
        }
        else if (kind == Output)
        {
            // the output section sits below: dragging up gives it more room
            owner.rack.uiOutputV = juce::jmax (0.0f, startLength - (float) e.getDistanceFromDragStartY());
        }
        else
        {
            // the sends sit after the divider: dragging it up (left) gives them more room
            const auto delta = (float) (owner.vertical ? e.getDistanceFromDragStartY() : e.getDistanceFromDragStartX());
            (owner.vertical ? owner.rack.uiSendsV : owner.rack.uiSendsH) = juce::jmax (0.0f, startLength - delta);
        }

        owner.resized();
        owner.repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasDraggedSinceMouseDown())
            owner.session.markDirty();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (kind == Input)                       owner.rack.uiInputV = -1.0f;
        else if (kind == Output)                 owner.rack.uiOutputV = -1.0f;
        else if (owner.vertical)                 owner.rack.uiSendsV = -1.0f;
        else                                     owner.rack.uiSendsH = -1.0f;
        owner.resized();
        owner.repaint();
        owner.session.markDirty();
    }

    void paint (juce::Graphics& g) override
    {
        const bool hot = isMouseOver() || isMouseButtonDown();
        const bool across = owner.vertical || kind != Sends;   // horizontal grip
        auto b = getLocalBounds().toFloat();
        const auto grip = across ? b.withSizeKeepingCentre (juce::jmin (36.0f, b.getWidth() * 0.4f), 2.0f)
                                 : b.withSizeKeepingCentre (2.0f, juce::jmin (36.0f, b.getHeight() * 0.4f));
        g.setColour (hot ? theme::accent : theme::outline);
        g.fillRoundedRectangle (grip, 1.0f);
    }

private:
    RackComponent& owner;
    Kind kind;
    float startLength = 0.0f;
};

//==============================================================================
RackComponent::RackComponent (Session& s, EditController& e, Rack& r, int i)
    : session (s), edit (e), rack (r), index (i), sendsPanel (s, e, r)
{
    // plugins and sends live in viewports: what does not fit scrolls (wheel / two fingers)
    for (auto* vp : { &slotView, &sendsView })
    {
        vp->setScrollBarThickness (5);
        vp->setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never);
    }
    slotView.setViewedComponent (&slotHolder, false);
    sendsView.setViewedComponent (&sendsPanel, false);
    addAndMakeVisible (slotView);
    addChildComponent (sendsView);

    nameLabel.setText (rack.name, juce::dontSendNotification);
    nameLabel.setFont (theme::font (14.0f, true));
    nameLabel.setEditable (false, true, false);
    nameLabel.onTextChange = [this] { rack.name = nameLabel.getText(); session.markDirty(); };
    addAndMakeVisible (nameLabel);

    // clicks, drags and right-clicks on the name act on the rack (double-click still renames)
    nameLabel.addMouseListener (this, false);

    for (auto* box : { &inputBox, static_cast<juce::ComboBox*> (&outputBox) })
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
        const auto linked = edit.linkedRacks (rack, mods, EditController::Link::routing);
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
        const auto linked = edit.linkedRacks (rack, mods, EditController::Link::routing);
        auto& engine = session.getEngine();

        if (id >= 4000)
        {
            // into a bus (refused where it would make a loop)
            const bool ok = engine.setRackOutputBus (rack, id - 4000);

            for (auto* r : linked)
                engine.setRackOutputBus (*r, id - 4000);

            if (! ok)
                fillRoutingBoxes();

            session.markDirty();
            return;
        }

        if (rack.outputBus.load() > 0)
            engine.setRackOutputBus (rack, 0);
        for (auto* r : linked)
            if (r->outputBus.load() > 0)
                engine.setRackOutputBus (*r, 0);
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
        slotHolder.addAndMakeVisible (slot);
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

        // Alt: same value (Alt+Shift: relative); edit-group members always follow relatively
        for (auto* r : edit.linkedRacks (rack, mods))
            r->gainDb = mods.isShiftDown() || ! edit.linkedBySelection (rack, *r, mods)
                          ? juce::jlimit (-60.0f, 12.0f, r->gainDb.load() + delta) : v;
    };
    gain.onDragEnd = [this] { gainDragging = false; session.markDirty(); };
    addAndMakeVisible (gain);

    // input trim, at the start of the rack (same editing rules as the output gain)
    inputGain.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    inputGain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 14);
    inputGain.setRange (Rack::minInputGainDb, Rack::maxInputGainDb, 0.1);
    inputGain.setDoubleClickReturnValue (true, 0.0, {});
    inputGain.setVelocityModeParameters (1.0, 1, 0.0, false);
    inputGain.textFromValueFunction = [] (double v) { return "IN " + juce::String (v, 1); };
    inputGain.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("-+.0123456789").getDoubleValue(); };
    inputGain.setColour (juce::Slider::rotarySliderFillColourId, rack.colour);
    inputGain.setValue (rack.inputGainDb.load(), juce::dontSendNotification);
    inputGain.updateText();
    inputGain.onDragStart = [this] { inGainDragMods = juce::ModifierKeys::getCurrentModifiers(); inGainDragging = true; };
    inputGain.onValueChange = [this]
    {
        const auto v = (float) inputGain.getValue();
        const auto delta = v - rack.inputGainDb.load();
        const auto mods = inGainDragging ? inGainDragMods : juce::ModifierKeys::getCurrentModifiers();
        rack.inputGainDb = v;

        for (auto* r : edit.linkedRacks (rack, mods))
            r->inputGainDb = mods.isShiftDown() || ! edit.linkedBySelection (rack, *r, mods)
                               ? juce::jlimit (Rack::minInputGainDb, Rack::maxInputGainDb, r->inputGainDb.load() + delta) : v;
    };
    inputGain.onDragEnd = [this] { inGainDragging = false; session.markDirty(); };
    addAndMakeVisible (inputGain);

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
    gain.setTooltip ("Output gain, after the plugins  (Alt: same on all selected racks, Alt+Shift: relative)");
    inputGain.setTooltip ("Input gain, before the plugins (" + juce::String ((int) Rack::minInputGainDb) + ".. +"
                          + juce::String ((int) Rack::maxInputGainDb) + " dB, double-click = 0)  (Alt: same on all selected racks, Alt+Shift: relative)");
    inputBox.setTooltip ("Input  (Alt: same on all selected, Alt+Shift: consecutive from here)");
    outputBox.onPopup = [this] { showOutputMenu(); };

    infoLabel.setFont (theme::font (10.5f));
    infoLabel.setColour (juce::Label::textColourId, theme::textDim);
    infoLabel.setJustificationType (juce::Justification::topLeft);
    infoLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (infoLabel);


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
    splitHandle = std::make_unique<SplitHandle> (*this, SplitHandle::Sends);
    inputHandle = std::make_unique<SplitHandle> (*this, SplitHandle::Input);
    outputHandle = std::make_unique<SplitHandle> (*this, SplitHandle::Output);
    addChildComponent (*splitHandle);
    addChildComponent (*inputHandle);
    addChildComponent (*outputHandle);

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
    // racks get thinner standing up (stacked controls) than lying down (two header rows)
    return juce::roundToInt ((float) (vertical ? baseWidth : baseHeight) * (vertical ? rack.uiSize : juce::jmax (0.5f, rack.uiSize)));
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

    // narrow racks: "IN 1-2" / "IN 1" (the dash says stereo)
    const juce::String st = compactRouting ? "" : " st", mono = compactRouting ? "" : " mono";

    inputBox.addItem ("IN: none", 1);
    for (int c = 0; c + 1 < ins.size(); c += 2)
        if (ins[c].isNotEmpty() && ins[c + 1].isNotEmpty())
            inputBox.addItem ("IN " + juce::String (c + 1) + "-" + juce::String (c + 2) + st, 1000 + c);
    for (int c = 0; c < ins.size(); ++c)
        if (ins[c].isNotEmpty())
            inputBox.addItem ("IN " + juce::String (c + 1) + mono, 2000 + c);

    outputBox.addItem ("OUT: none", 1);
    for (int c = 0; c + 1 < outs.size(); c += 2)
        if (outs[c].isNotEmpty() && outs[c + 1].isNotEmpty())
            outputBox.addItem ("OUT " + juce::String (c + 1) + "-" + juce::String (c + 2) + st, 1000 + c);
    for (int c = 0; c < outs.size(); ++c)
        if (outs[c].isNotEmpty())
            outputBox.addItem ("OUT " + juce::String (c + 1) + mono, 2000 + c);

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

    // buses: an output can go into one (the bus sums it); a bus takes its input from its feeds
    const bool routing = engine.isRoutingEnabled();
    bool anyBus = false;

    for (int k = 1; routing && k <= Rack::maxBuses; ++k)
    {
        auto* bus = engine.findBus (k);

        if (bus == nullptr || bus == &rack)
            continue;

        if (! anyBus)
        {
            outputBox.addSectionHeading ("To bus");
            anyBus = true;
        }

        const auto letter = juce::String::charToString ((juce::juce_wchar) ('A' + k - 1));
        const bool defaultName = bus->name == "Bus " + letter;   // "Bus A" once, not "Bus A  Bus A"
        outputBox.addItem (juce::String::fromUTF8 (compactRouting && ! defaultName ? "\xe2\x96\xb6 " : "\xe2\x96\xb6 Bus ") + letter
                               + (defaultName || tinyRouting ? juce::String() : "  " + bus->name), 4000 + k);
        outputBox.setItemEnabled (4000 + k, engine.canRouteToBus (rack, k));
    }

    inputBox.setEnabled (! rack.isActiveBus);

    if (rack.isActiveBus)
    {
        inputBox.setText (compactRouting ? "IN: bus" : "IN: bus (sends + routed racks)", juce::dontSendNotification);
    }

    const auto in = rack.inputChannel.load();

    if (rack.isActiveBus) {}
    else if (const auto src = rack.inputRackId.load(); src >= 0 && engine.findRack (src) != nullptr)
        inputBox.setSelectedId (3000 + src, juce::dontSendNotification);
    else
        inputBox.setSelectedId (in < 0 ? 1 : (rack.stereoInput.load() ? 1000 + in : 2000 + in), juce::dontSendNotification);

    if (rack.isActiveBus) {}
    else if (inputBox.getSelectedId() == 0 && rack.inputRackId < 0 && rack.stereoInput.load()
        && juce::isPositiveAndBelow (in, ins.size()) && ins[in].isNotEmpty())
        inputBox.setText ("IN " + juce::String (in + 1) + (compactRouting ? " (m)" : " (mono device)"), juce::dontSendNotification);   // L copied to R
    else if (inputBox.getSelectedId() == 0 && rack.inputRackId < 0)
        inputBox.setText ("IN " + juce::String (in + 1) + (rack.stereoInput.load() ? st : mono) + (tinyRouting ? "" : " (off)"), juce::dontSendNotification);

    const auto out = rack.outputChannel.load();

    if (const auto ob = rack.outputBus.load(); routing && ob > 0)
    {
        const auto letter = juce::String::charToString ((juce::juce_wchar) ('A' + ob - 1));

        if (engine.findBus (ob) != nullptr && rack.outBusRack != nullptr)
            outputBox.setSelectedId (4000 + ob, juce::dontSendNotification);
        else
            outputBox.setText (juce::String::fromUTF8 ("\xe2\x96\xb6 Bus ") + letter + " (missing: silent)", juce::dontSendNotification);
    }
    else
    {
        outputBox.setSelectedId (out < 0 ? 1 : (rack.stereoOutput.load() ? 1000 + out : 2000 + out), juce::dontSendNotification);

        if (outputBox.getSelectedId() == 0)
            outputBox.setText ("OUT " + juce::String (out + 1) + (rack.stereoOutput.load() ? st : mono) + (tinyRouting ? "" : " (off)"), juce::dontSendNotification);
    }

    // more destinations: "OUT 1-2 +2", every one listed in the tooltip
    juce::StringArray dests;
    dests.add (outputBox.getText());
    int extras = 0;

    for (auto c : rack.getExtraOutputs())
    {
        if (c > Rack::outBus && ! routing)
            continue;   // a bus while buses are off: kept, not played
        dests.add (outputName (c));
        ++extras;
    }

    if (extras > 0)
        outputBox.setText (outputBox.getText() + (tinyRouting ? " +" : "  +") + juce::String (extras), juce::dontSendNotification);

    outputBox.setTooltip ((extras > 0 ? "Outputs: " + dests.joinIntoString (", ") + "\n" : juce::String())
                          + "Click: main output (Alt: same on all selected, Alt+Shift: consecutive from here)\n"
                            "Ctrl-click or \"Additional outputs\": add / remove more outputs");
}

juce::String RackComponent::outputName (int code) const
{
    if (code > Rack::outBus)
    {
        const auto k = code - Rack::outBus;
        auto* bus = session.getEngine().findBus (k);
        return juce::String::fromUTF8 ("\xe2\x96\xb6 Bus ") + juce::String::charToString ((juce::juce_wchar) ('A' + k - 1))
               + (bus == nullptr ? " (missing)" : "");
    }

    if (code >= Rack::outMono)
        return "OUT " + juce::String (code - Rack::outMono + 1);

    const auto ch = code - Rack::outStereo;
    return "OUT " + juce::String (ch + 1) + "-" + juce::String (ch + 2);
}

void RackComponent::toggleExtraOutput (int code, const juce::ModifierKeys& mods)
{
    auto& engine = session.getEngine();
    juce::Array<Rack*> all (edit.linkedRacks (rack, mods, EditController::Link::routing));
    all.add (&rack);

    const auto mine = rack.getExtraOutputs();
    const bool add = std::find (mine.begin(), mine.end(), code) == mine.end();

    for (auto* r : all)
    {
        auto codes = r->getExtraOutputs();
        codes.erase (std::remove (codes.begin(), codes.end(), code), codes.end());

        if (add)
            codes.push_back (code);

        engine.setRackExtraOutputs (*r, codes);
    }

    session.markDirty();
    fillRoutingBoxes();
}

void RackComponent::showOutputMenu()
{
    auto* root = outputBox.getRootMenu();
    if (root == nullptr)
        return;

    const bool routing = session.getEngine().isRoutingEnabled();
    const auto main = rack.mainOutputCode (routing);
    const auto extras = rack.getExtraOutputs();
    auto isExtra = [&extras] (int id) { return std::find (extras.begin(), extras.end(), id) != extras.end(); };

    juce::PopupMenu menu, more;
    juce::PopupMenu::MenuItemIterator it (*root);

    while (it.next())
    {
        const auto& item = it.getItem();

        if (item.isSectionHeader)
        {
            menu.addSectionHeader (item.text);
            more.addSectionHeader (item.text);
            continue;
        }

        if (item.itemID <= 0)
            continue;

        const bool extra = isExtra (item.itemID);
        juce::PopupMenu::Item m (item.text + (extra ? "   (+)" : ""));
        m.itemID = item.itemID;
        m.isEnabled = item.isEnabled;
        m.isTicked = item.itemID == main || extra;
        menu.addItem (m);

        if (item.itemID != Rack::outNone)
        {
            juce::PopupMenu::Item a (item.text);
            a.itemID = 100000 + item.itemID;
            a.isEnabled = item.isEnabled && item.itemID != main;
            a.isTicked = extra;
            more.addItem (a);
        }
    }

    menu.addSeparator();
    menu.addSubMenu ("Additional outputs" + (extras.empty() ? juce::String() : "  (" + juce::String ((int) extras.size()) + ")"), more);
    menu.addItem (99999, "Clear additional outputs", ! extras.empty());

    juce::Component::SafePointer<RackComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&outputBox).withMinimumWidth (outputBox.getWidth()),
        [safe] (int result)
        {
            if (safe == nullptr || result == 0)
                return;

            auto& self = *safe;
            const auto mods = juce::ModifierKeys::getCurrentModifiersRealtime();

            if (result == 99999)
            {
                juce::Array<Rack*> all (self.edit.linkedRacks (self.rack, mods, EditController::Link::routing));
                all.add (&self.rack);
                for (auto* r : all)
                    self.session.getEngine().setRackExtraOutputs (*r, {});
                self.session.markDirty();
                self.fillRoutingBoxes();
                return;
            }

            if (result > 100000)
            {
                self.toggleExtraOutput (result - 100000, mods);
                return;
            }

            // Ctrl-click (Pro Tools): add / remove this destination instead of replacing the main one
            if (mods.isCtrlDown() && result != Rack::outNone && result != self.rack.mainOutputCode (self.session.getEngine().isRoutingEnabled()))
            {
                self.toggleExtraOutput (result, mods);
                return;
            }

            // the new main output must not stay an extra as well
            if (auto codes = self.rack.getExtraOutputs(); std::find (codes.begin(), codes.end(), result) != codes.end())
            {
                codes.erase (std::remove (codes.begin(), codes.end(), result), codes.end());
                self.session.getEngine().setRackExtraOutputs (self.rack, codes);
            }

            self.outputBox.setSelectedId (0, juce::dontSendNotification);
            self.outputBox.setSelectedId (result, juce::sendNotificationSync);
        });
}

void RackComponent::refresh()
{
    nameLabel.setText (rack.name, juce::dontSendNotification);
    gain.setValue (rack.gainDb.load(), juce::dontSendNotification);
    inputGain.setValue (rack.inputGainDb.load(), juce::dontSendNotification);
    muteButton.setToggleState (rack.muted.load(), juce::dontSendNotification);
    soloButton.setToggleState (rack.solo.load(), juce::dontSendNotification);
    bypassButton.setToggleState (rack.bypassed.load(), juce::dontSendNotification);
    gain.setColour (juce::Slider::rotarySliderFillColourId, rack.colour);
    inputGain.setColour (juce::Slider::rotarySliderFillColourId, rack.colour);
    fillRoutingBoxes();

    // the session's view changed (VIEW menu): every section may move
    if (sendsPanel.update() || sendsPanel.isVisible() != sendsPanel.wantsToBeShown() || ! (session.viewFor (rack) == laidOutView))
    {
        resized();
    }
    sendsPanel.repaint();

    for (auto* s : slots)
        s->repaint();

    repaint();
}

void RackComponent::updateMeters()
{
    for (int c = 0; c < 2; ++c)
    {
        // never an infinite level: it would never fall back (inf x 0.82 = inf)
        auto finite = [] (float v) { return std::isfinite (v) ? juce::jmin (v, 4.0f) : 4.0f; };
        inLevel[c]  = juce::jmax (finite (rack.inMeter[c].readAndReset()),  finite (inLevel[c] * 0.82f));
        outLevel[c] = juce::jmax (finite (rack.outMeter[c].readAndReset()), finite (outLevel[c] * 0.82f));
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

    updateBadges();

    repaint (inMeterArea);
    repaint (outMeterArea);

    if ((rack.automixGroup.load() > 0) == automixArea.isEmpty())
        resized();

    if (sendsPanel.update() || sendsPanel.isVisible() != sendsPanel.wantsToBeShown())
        resized();   // buses added/removed, routing switched (also from undo or another rack)   // group changed elsewhere (scene, Alt on another rack)

    if (rack.automixGroup.load() > 0)
    {
        automixDb = rack.automixGainDb.load();
        repaint (automixArea);
        repaint (automixMeterArea);
    }

    // values changed from elsewhere (Alt on another rack, MIDI, scenes) show up here
    if (! gain.isMouseButtonDown() && std::abs ((float) gain.getValue() - rack.gainDb.load()) > 0.001f)
        gain.setValue (rack.gainDb.load(), juce::dontSendNotification);

    if (! inputGain.isMouseButtonDown() && std::abs ((float) inputGain.getValue() - rack.inputGainDb.load()) > 0.001f)
        inputGain.setValue (rack.inputGainDb.load(), juce::dontSendNotification);

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

    // the whole rack in its colour (greyish in the dark theme), less when muted/bypassed
    g.setColour (theme::tinted (selected ? theme::panelLight : theme::panel, rack.colour,
                                (active ? 1.0f : 0.4f) * (theme::light ? 0.2f : 0.16f)));
    g.fillRoundedRectangle (b, 6.0f);

    // active edit group: the whole rack is framed in the group colour, so it is clear at a glance
    // that touching this rack also changes the others (suspended group = no frame)
    const bool groupLinked = rack.editGroup > 0 && session.isEditGroupActive (rack.editGroup);

    if (groupLinked)
    {
        g.setColour (theme::groupColour (rack.editGroup));
        g.drawRoundedRectangle (b.reduced (1.25f), 6.0f, 2.5f);
    }

    if (selected)
    {
        g.setColour (theme::accent);
        g.drawRoundedRectangle (b.reduced (groupLinked ? 4.0f : 1.0f), groupLinked ? 4.0f : 6.0f, 2.0f);
    }

    {
        auto strip = vertical ? b.removeFromTop (6.0f) : b.removeFromLeft (6.0f);

        // inside a folder: the folder's colour on the outer half of the strip (like Ableton)
        if (auto* folder = rack.folderId >= 0 ? session.getEngine().findRack (rack.folderId) : nullptr)
        {
            g.setColour (folder->colour);
            g.fillRoundedRectangle (strip, 3.0f);
            strip = vertical ? strip.withTrimmedTop (3.0f) : strip.withTrimmedLeft (3.0f);
        }

        g.setColour (active ? rack.colour : rack.colour.withSaturation (0.1f).darker (0.4f));
        g.fillRoundedRectangle (strip, rack.folderId >= 0 ? 1.5f : 3.0f);
    }

    // folder: open / closed triangle before the name
    if (! folderArea.isEmpty())
    {
        auto a = folderArea.toFloat().withSizeKeepingCentre (10.0f, 10.0f);
        juce::Path p;
        if (rack.folderOpen) p.addTriangle (a.getX(), a.getY() + 2.0f, a.getRight(), a.getY() + 2.0f, a.getCentreX(), a.getBottom() - 1.0f);
        else                 p.addTriangle (a.getX() + 2.0f, a.getY(), a.getX() + 2.0f, a.getBottom(), a.getRight() - 1.0f, a.getCentreY());
        g.setColour (rack.colour);
        g.fillPath (p);
    }

    g.setColour (theme::textDim);
    g.setFont (theme::font (isNarrow() ? 10.0f : 11.0f, true));
    g.drawText (juce::String (index + 1).paddedLeft ('0', 2), indexArea, juce::Justification::centredLeft);

    // edit group: letter in the group colour, filled while the group links edits, outlined when suspended
    if (const auto group = rack.editGroup; group > 0 && ! groupArea.isEmpty())
    {
        const bool on = session.isEditGroupActive (group);
        const auto c = theme::groupColour (group);
        auto a = groupArea.toFloat();
        g.setColour (on ? c : c.withAlpha (0.12f));
        g.fillRoundedRectangle (a, 3.0f);
        g.setColour (c);
        if (! on) g.drawRoundedRectangle (a.reduced (0.5f), 3.0f, 1.0f);
        g.setColour (on ? juce::Colour (0xff101214) : c);
        g.setFont (theme::font (10.5f, true));
        g.drawText ((a.getWidth() > 30.0f ? "GRP " : "") + theme::groupLetter (group), a, juce::Justification::centred, false);
    }

    // automix: group letter + the gain the automixer gives this rack right now (0 .. -40 dB)
    if (const auto group = rack.automixGroup.load(); group > 0 && ! automixArea.isEmpty())
    {
        const bool on = session.getEngine().automixEnabled.load();
        auto a = automixArea.toFloat();
        g.setColour (theme::meterBg);
        g.fillRoundedRectangle (a, 3.0f);

        const auto frac = on ? juce::jlimit (0.0f, 1.0f, 1.0f + automixDb / 40.0f) : 1.0f;
        g.setColour ((on ? theme::green : theme::textDim).withAlpha (on ? 0.75f : 0.35f));
        g.fillRoundedRectangle (a.withWidth (a.getWidth() * frac), 3.0f);

        g.setColour (theme::text);
        g.setFont (theme::font (9.5f, true));
        g.drawText ((a.getWidth() < 70.0f ? "" : "AM ") + juce::String::charToString ((juce::juce_wchar) ('A' + group - 1))
                      + (on ? "  " + (automixDb <= -59.5f ? juce::String ("-inf") : juce::String (automixDb, 1)) : juce::String ("  off")),
                    a.reduced (4.0f, 0.0f), juce::Justification::centredLeft, false);
    }

    paintBadges (g);

    // meters: input (after the input gain) next to the IN knob, output next to the output gain
    auto drawPair = [&g] (juce::Rectangle<int> area, const float* level)
    {
        if (area.isEmpty())
            return;

        auto m = area.toFloat();
        const float w = (m.getWidth() - 1.0f) / 2.0f;
        theme::drawMeter (g, m.withWidth (w), level[0]);
        theme::drawMeter (g, m.withTrimmedLeft (w + 1.0f), level[1]);
    };

    drawPair (inMeterArea, inLevel);
    drawPair (outMeterArea, outLevel);

    // dB scale beside the big meters (same mapping as theme::drawMeter: -60..0 dB)
    auto drawScale = [&g] (juce::Rectangle<int> scale, juce::Rectangle<int> meter)
    {
        if (scale.isEmpty() || meter.isEmpty())
            return;

        g.setFont (theme::font (8.5f));
        float lastY = -100.0f;

        for (int db : { 0, -6, -12, -18, -24, -36, -48, -60 })
        {
            const auto y = (float) meter.getY() + (float) meter.getHeight() * (float) -db / 60.0f;

            if (y - lastY < 10.0f)
                continue;   // short meter: fewer marks
            lastY = y;

            g.setColour (theme::textDim.withAlpha (0.5f));
            g.drawHorizontalLine (juce::roundToInt (y), (float) scale.getX(), (float) scale.getX() + 3.0f);
            g.setColour (theme::textDim);
            g.drawText (juce::String (-db), juce::Rectangle<float> ((float) scale.getX() + 3.0f, y - 5.0f, (float) scale.getWidth() - 3.0f, 10.0f),
                        juce::Justification::centredLeft, false);
        }
    };

    drawScale (inScaleArea, inMeterArea);
    drawScale (outScaleArea, outMeterArea);

    // automix gain meter (0 dB = full, -40 dB = empty), in the colour of the group
    if (const auto group = rack.automixGroup.load(); group > 0 && ! automixMeterArea.isEmpty())
    {
        const bool on = session.getEngine().automixEnabled.load();
        auto a = automixMeterArea.toFloat();
        g.setColour (theme::meterBg);
        g.fillRoundedRectangle (a, 1.5f);

        const auto norm = on ? juce::jlimit (0.0f, 1.0f, 1.0f + automixDb / 40.0f) : 1.0f;
        const auto c = AutomixerPanel::groupColour (group);
        g.setColour (on ? c : c.withSaturation (0.1f).withAlpha (0.4f));
        g.fillRoundedRectangle (a.withTrimmedTop (a.getHeight() * (1.0f - norm)), 1.5f);

        g.setColour (norm * a.getHeight() > 12.0f ? theme::meterBg : c);   // the letter stays readable on an empty meter
        g.setFont (theme::font (9.0f, true));
        g.drawText (juce::String::charToString ((juce::juce_wchar) ('A' + group - 1)),
                    a.removeFromBottom (12.0f), juce::Justification::centred, false);
    }
}

void RackComponent::updateBadges()
{
    std::vector<Badge> b;
    const auto sr = juce::jmax (1.0, rack.getSampleRate());

    if (peakLoad > 0.9f)
        b.push_back ({ juce::String::fromUTF8 ("\xe2\x9a\xa0 CPU spikes"), juce::String::fromUTF8 ("\xe2\x9a\xa0 CPU"),
                       "The plugins of this rack came close to the whole buffer time", theme::amber });

    if (rack.isFolder)
    {
        const auto n = session.numFolderMembers (rack);
        b.push_back ({ "FOLDER " + juce::String (n) + (n == 1 ? " rack" : " racks"), "F" + juce::String (n),
                       "Folder: its racks play into it (Ableton group track). Click the triangle to open / close it.",
                       rack.colour });
    }

    if (rack.isActiveBus && ! rack.isFolder)
    {
        const auto letter = juce::String::charToString ((juce::juce_wchar) ('A' + rack.busIndex.load() - 1));
        b.push_back ({ "BUS " + letter, "BUS " + letter, "Bus " + letter + ": its input is the sum of the sends and racks routed here",
                       theme::groupColour (rack.busIndex.load()) });
    }

    if (const auto group = rack.latencyGroup.load(); group > 0)
    {
        const auto letter = juce::String::charToString ((juce::juce_wchar) ('A' + group - 1));
        const auto compMs = 1000.0 * rack.compensationSamples.load() / sr;
        b.push_back ({ "LG " + letter + (compMs > 0.05 ? " +" + juce::String (compMs, 1) + " ms" : juce::String()), "LG " + letter,
                       "Latency group " + letter + (compMs > 0.05 ? ": delayed " + juce::String (compMs, 2) + " ms to line up with the group" : juce::String()),
                       theme::accent });
    }

    if (const auto t = rack.tempoIndex.load(); t > 0 && t < session.getEngine().tempo.getNumTempos())
    {
        auto& tempo = session.getEngine().tempo;
        const auto bpm = juce::String (tempo.getBpm (t), 1);
        b.push_back ({ juce::String::fromUTF8 ("\xe2\x99\xa9 ") + TempoEngine::shortName (t) + " " + bpm, TempoEngine::shortName (t),
                       "Plugins of this rack play with " + tempo.getConfig (t).name + " (" + bpm + " BPM)", tempoColour (t) });
    }

    const bool changedPresence = b.empty() != badges.empty();
    bool same = b.size() == badges.size();
    for (size_t i = 0; same && i < b.size(); ++i)
        same = b[i].text == badges[i].text && b[i].colour == badges[i].colour;

    if (same)
        return;

    badges = std::move (b);

    if (changedPresence && vertical)
        resized();   // vertical racks give the badges a row of their own when needed

    repaint (badgeArea);
}

void RackComponent::paintBadges (juce::Graphics& g)
{
    if (badges.empty() || badgeArea.isEmpty())
        return;

    const auto font = theme::font (badgeArea.getHeight() < 16 ? 9.5f : 10.0f, true);
    const float gap = 4.0f, pad = 5.0f;

    auto widthOf = [&font] (const juce::String& t) { return juce::GlyphArrangement::getStringWidth (font, t); };

    // full texts if they all fit, else short ones (every badge stays visible)
    auto total = [&] (bool shortForm)
    {
        float w = 0.0f;
        for (auto& b : badges)
            w += widthOf (shortForm ? b.shortText : b.text) + 2.0f * pad + gap;
        return w - gap;
    };

    const bool shortForm = total (false) > (float) badgeArea.getWidth();
    auto a = badgeArea.toFloat().reduced (0.0f, 1.0f);
    g.setFont (font);

    for (auto& b : badges)
    {
        const auto& text = shortForm ? b.shortText : b.text;
        const auto w = juce::jmin (a.getWidth(), widthOf (text) + 2.0f * pad);
        if (w <= 8.0f) break;

        auto chip = a.removeFromLeft (w);
        a.removeFromLeft (gap);
        g.setColour (b.colour.withAlpha (0.16f));
        g.fillRoundedRectangle (chip, 3.0f);
        g.setColour (b.colour.withAlpha (0.7f));
        g.drawRoundedRectangle (chip.reduced (0.5f), 3.0f, 1.0f);
        g.setColour (theme::light ? b.colour.darker (0.5f) : b.colour);
        g.drawText (text, chip, juce::Justification::centred, false);
    }
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
    // what the session's view shows (VIEW menu): I/O selectors, input, inserts, sends, info
    const auto view = session.viewFor (rack);   // the session's VIEW, or this rack's own sections
    laidOutView = view;
    inputBox.setVisible (view.io);
    outputBox.setVisible (view.io);
    infoLabel.setVisible (view.info);
    showInserts = view.inserts;

    groupArea = {};
    folderArea = {};
    automixArea = {};
    automixMeterArea = {};
    const bool inGroup = rack.editGroup > 0;
    const bool automixed = rack.automixGroup.load() > 0;

    if (isNarrow() != compactRouting || isTiny() != tinyRouting)
    {
        compactRouting = isNarrow();
        tinyRouting = isTiny();
        fillRoutingBoxes();
    }


    auto layoutButtons = [this] (juce::Rectangle<int> buttons)
    {
        const int bh = (buttons.getHeight() - 4) / 3;
        muteButton.setBounds (buttons.removeFromTop (bh));
        buttons.removeFromTop (2);
        soloButton.setBounds (buttons.removeFromTop (bh));
        buttons.removeFromTop (2);
        bypassButton.setBounds (buttons.removeFromTop (bh));
    };

    if (vertical)
    {
        resizeHandle->setBounds (getWidth() - 5, 0, 5, getHeight());
        const bool narrow = isNarrow(), tiny = isTiny();
        auto r = getLocalBounds().reduced (tiny ? 3 : narrow ? 5 : 8, 6).withTrimmedTop (8).withTrimmedRight (2);

        // name row: the name gets all the room; on narrow racks the number moves to the row below
        auto nameRow = r.removeFromTop (20);
        menuButton.setBounds (nameRow.removeFromRight (tiny ? 16 : 28));
        if (! narrow) indexArea = nameRow.removeFromLeft (22);
        if (rack.isFolder) folderArea = nameRow.removeFromLeft (tiny ? 12 : 16);
        nameLabel.setFont (theme::font (tiny ? 11.5f : narrow ? 13.0f : 14.0f, true));
        nameLabel.setBounds (nameRow);

        {
            auto statusRow = r.removeFromTop (16);
            if (narrow) indexArea = statusRow.removeFromLeft (18);

            if (inGroup)
            {
                groupArea = statusRow.removeFromLeft (narrow ? 18 : 40).reduced (0, 1);
                statusRow.removeFromLeft (tiny ? 2 : 4);
            }

            if (automixed)
                automixArea = (narrow ? statusRow : statusRow.removeFromRight (statusRow.getWidth() / 2)).reduced (0, 2);

            badgeArea = narrow ? juce::Rectangle<int>() : statusRow;
        }

        if (! narrow && ! badges.empty() && badgeArea.getWidth() < 120)
        {
            r.removeFromTop (2);
            badgeArea = r.removeFromTop (16);   // no room beside the group / automix: a row of its own
        }

        r.removeFromTop (2);

        if (view.io)
        {
            inputBox.setBounds (r.removeFromTop (21));
            r.removeFromTop (3);
            outputBox.setBounds (r.removeFromTop (21));
            r.removeFromTop (2);
        }

        // bottom: info, fader + output meter + M/S/B
        infoLabel.setFont (theme::font (tiny ? 9.0f : narrow ? 9.5f : 10.5f));
        if (view.info)
            infoLabel.setBounds (r.removeFromBottom (tiny ? 34 : narrow ? 38 : 42));

        if (narrow)
            badgeArea = r.removeFromBottom (15);

        // output section (meter + gain + M/S/B), as tall as the divider above it says; taller
        // than faderFrom = big meter with scale + fader, like a console channel
        const int autoOut = tiny ? 98 : 78, minOut = tiny ? 72 : 60;
        outputLength = juce::jlimit (minOut, juce::jmax (minOut, r.getHeight() - 7 - 40),
                                     rack.uiOutputV >= 0.0f ? juce::roundToInt (rack.uiOutputV) : autoOut);
        auto controls = r.removeFromBottom (outputLength);
        outputHandle->setVisible (true);
        outputHandle->setBounds (r.removeFromBottom (7));
        const bool outFader = outputLength >= 130;
        outScaleArea = {};
        inScaleArea = {};

        // input trim + its meter, as tall as the divider below it says (0 = hidden)
        {
            const int autoIn = narrow ? 54 : 40;
            const int maxIn = juce::jmax (0, juce::jmin (320, r.getHeight() - 7));
            int inLen = rack.uiInputV >= 0.0f ? juce::roundToInt (rack.uiInputV) : autoIn;
            inLen = view.input ? juce::jlimit (0, maxIn, inLen < 14 ? 0 : inLen) : 0;
            inputLength = inLen;

            auto row = r.removeFromTop (inLen);
            inputGain.setVisible (inLen > 0);

            if (inLen >= 64)
            {
                // big meter with its scale, the trim as a fader beside it
                setFaderStyle (inputGain, true);
                row.reduce (0, 2);
                inMeterArea = row.removeFromLeft (tiny ? 10 : narrow ? 16 : 24).withTrimmedBottom (16);
                if (! tiny) inScaleArea = row.removeFromLeft (narrow ? 14 : 18).withTrimmedBottom (16);
                row.removeFromLeft (2);
                inputGain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jmax (30, row.getWidth()), 14);
                inputGain.setBounds (row);
            }
            else if (inLen > 0)
            {
                setFaderStyle (inputGain, false);
                inMeterArea = row.removeFromLeft (tiny ? 8 : 12).reduced (0, 2);
                row.removeFromLeft (tiny ? 2 : 4);

                if (narrow)          inputGain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, row.getWidth(), 14);
                else if (inLen < 24) inputGain.setTextBoxStyle (juce::Slider::TextBoxRight, false, juce::jmin (64, row.getWidth() / 2), inLen);
                else                 inputGain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 16);

                inputGain.setBounds (row);
            }
            else
            {
                inMeterArea = {};
            }

            inputHandle->setVisible (view.input);
            if (view.input)
                inputHandle->setBounds (r.removeFromTop (7));
        }

        layoutSends (r);   // sends just above the fader, as on a console

        if (tiny)
        {
            // too thin for buttons beside the fader: M S B in a row under it
            auto buttons = controls.removeFromBottom (18);
            const int bw = (buttons.getWidth() - 4) / 3;
            muteButton.setBounds (buttons.removeFromLeft (bw));
            buttons.removeFromLeft (2);
            soloButton.setBounds (buttons.removeFromLeft (bw));
            buttons.removeFromLeft (2);
            bypassButton.setBounds (buttons);
            controls.removeFromBottom (2);
        }
        else if (outFader)
        {
            // buttons at the top of their column, normal size
            auto column = controls.removeFromRight (narrow ? 22 : 28);
            layoutButtons (column.removeFromTop (78));
            controls.removeFromRight (narrow ? 2 : 4);
        }
        else
        {
            layoutButtons (controls.removeFromRight (narrow ? 22 : 28));
            controls.removeFromRight (narrow ? 2 : 4);
        }

        if (outFader)
        {
            setFaderStyle (gain, true);
            controls.reduce (0, 2);
            outMeterArea = controls.removeFromLeft (tiny ? 10 : narrow ? 16 : 24).withTrimmedBottom (16);
            if (! tiny) outScaleArea = controls.removeFromLeft (narrow ? 14 : 18).withTrimmedBottom (16);
        }
        else
        {
            setFaderStyle (gain, false);
            outMeterArea = controls.removeFromLeft (tiny ? 8 : 12).reduced (0, 2);
        }

        if (automixed)
        {
            controls.removeFromLeft (2);
            automixMeterArea = controls.removeFromLeft (narrow ? 6 : 11).reduced (0, 2);
            if (outFader) automixMeterArea = automixMeterArea.withTrimmedBottom (14);
        }

        controls.removeFromLeft (narrow ? 1 : 4);
        gain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jmax (tiny ? 30 : 40, controls.getWidth()), 14);
        gain.setBounds (controls);

        layoutSlots (r);
        return;
    }

    inputHandle->setVisible (false);
    outputHandle->setVisible (false);
    inScaleArea = outScaleArea = {};
    setFaderStyle (gain, false);
    setFaderStyle (inputGain, false);
    resizeHandle->setBounds (0, getHeight() - 5, getWidth(), 5);

    const bool compact = getHeight() < 72;   // shrunk rack: name + IN/OUT on two rows
    auto r = getLocalBounds().reduced (6, compact ? 4 : 6).withTrimmedLeft (8).withTrimmedBottom (2);

    auto header = r.removeFromLeft (290);
    auto nameRow = header.removeFromTop (compact ? 18 : 20);
    indexArea = nameRow.removeFromLeft (22);
    if (rack.isFolder) folderArea = nameRow.removeFromLeft (16);

    if (inGroup)
    {
        groupArea = nameRow.removeFromLeft (20).reduced (0, 2);
        nameRow.removeFromLeft (4);
    }

    menuButton.setBounds (nameRow.removeFromRight (28));
    nameRow.removeFromRight (4);
    badgeArea = {};
    if (compact)
        badgeArea = nameRow.removeFromRight (automixed ? 60 : 96);

    if (automixed)
    {
        automixArea = nameRow.removeFromRight (78).reduced (0, 3);
        nameRow.removeFromRight (4);
    }

    nameLabel.setFont (theme::font (14.0f, true));
    nameLabel.setBounds (nameRow);
    header.removeFromTop (3);

    if (! view.io)
    {
        if (! compact)
            badgeArea = header.removeFromTop (juce::jmin (16, header.getHeight()));
    }
    else if (compact)
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
        header.removeFromTop (5);
        badgeArea = header.removeFromTop (juce::jmin (16, header.getHeight()));
    }

    // input gain + its meter, then the plugins, then output meter + gain
    r.removeFromLeft (8);
    inputGain.setVisible (view.input);
    inMeterArea = {};

    if (view.input)
    {
        inputGain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 14);
        inputGain.setBounds (r.removeFromLeft (58));
        r.removeFromLeft (2);
        inMeterArea = r.removeFromLeft (12).reduced (0, 2);
        r.removeFromLeft (8);
    }

    infoLabel.setFont (theme::font (10.5f));
    if (view.info)
        infoLabel.setBounds (r.removeFromRight (104).reduced (4, 0));
    layoutButtons (r.removeFromRight (28));

    r.removeFromRight (4);
    gain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 14);
    gain.setBounds (r.removeFromRight (58));
    r.removeFromRight (2);

    if (automixed)
    {
        automixMeterArea = r.removeFromRight (11).reduced (0, 2);
        r.removeFromRight (3);
    }

    outMeterArea = r.removeFromRight (12).reduced (0, 2);
    r.removeFromRight (8);

    layoutSends (r);   // sends between the plugins and the fader

    layoutSlots (r);
}

void RackComponent::setFaderStyle (juce::Slider& s, bool fader)
{
    const auto style = fader ? juce::Slider::LinearVertical : juce::Slider::RotaryHorizontalVerticalDrag;

    if (s.getSliderStyle() != style)
    {
        s.setSliderStyle (style);
        s.setSliderSnapsToMousePosition (false);   // a fader never jumps to where you click (live!)
    }
}

void RackComponent::layoutSends (juce::Rectangle<int>& r)
{
    const bool show = sendsPanel.wantsToBeShown();
    splitHandle->setVisible (show);

    if (! show)
    {
        sendsView.setVisible (false);
        return;
    }

    constexpr int handle = 7;
    const auto sb = sendsView.getScrollBarThickness();

    // The divider goes all the way: 0 = only plugins, everything = only sends. Its position is
    // this rack's (rack.uiSendsV / H, -1 = automatic: up to 2 knob rows / 4 bars).
    auto lengthFor = [] (float user, int autoLen, int total)
    {
        if (user < 0.0f)
            return juce::jlimit (0, juce::jmax (0, total - 40), autoLen);

        auto len = juce::jlimit (0, total, juce::roundToInt (user));
        if (len < 10)           len = 0;       // snap to the ends
        if (total - len < 14)   len = total;
        return len;
    };

    if (! showInserts)
    {
        // no plugins in the view: the sends get all the room, no divider
        splitHandle->setVisible (false);
        auto area = r;
        r = r.withHeight (0).withWidth (vertical ? r.getWidth() : 0);
        const int across = vertical ? area.getWidth() : area.getWidth();
        sendsPanel.setFitHeight (area.getHeight());
        int innerW = across;
        int content = sendsPanel.getContentHeight (innerW);
        if (content > area.getHeight()) { innerW = across - sb; content = sendsPanel.getContentHeight (innerW); }
        sendsView.setVisible (true);
        sendsView.setBounds (area);
        sendsView.setScrollBarsShown (content > area.getHeight(), false, true, false);
        sendsPanel.setSize (juce::jmax (0, innerW), juce::jmax (content, area.getHeight()));
        return;
    }

    if (vertical)
    {
        const int w = r.getWidth();
        const int total = juce::jmax (0, r.getHeight() - handle);
        sendsPanel.setFitHeight (0);
        const auto row = SendsPanel::getRowHeight (w);
        const auto autoLen = juce::jmin (sendsPanel.getContentHeight (w), SendsPanel::usesKnobs (w) ? 2 * row : 4 * row);
        const auto len = lengthFor (rack.uiSendsV, autoLen, total);

        auto area = r.removeFromBottom (len);
        splitHandle->setBounds (r.removeFromBottom (handle));
        sendsView.setVisible (len > 0);

        sendsPanel.setFitHeight (len);
        int innerW = w;
        int content = sendsPanel.getContentHeight (innerW);

        if (content > len)
        {
            innerW = w - sb;
            content = sendsPanel.getContentHeight (innerW);
        }

        sendsView.setBounds (area);
        sendsView.setScrollBarsShown (content > len, false, true, false);
        sendsPanel.setSize (innerW, juce::jmax (content, len));
    }
    else
    {
        const int total = juce::jmax (0, r.getWidth() - handle - 2);
        const auto autoLen = r.getWidth() > 700 ? 110 : 86;
        const auto len = lengthFor (rack.uiSendsH, autoLen, total);

        auto area = r.removeFromRight (len);
        splitHandle->setBounds (r.removeFromRight (handle));
        r.removeFromRight (2);
        sendsView.setVisible (len > 0);

        sendsPanel.setFitHeight (area.getHeight());
        int innerW = len;
        int content = sendsPanel.getContentHeight (innerW);

        if (content > area.getHeight())
        {
            innerW = len - sb;
            content = sendsPanel.getContentHeight (innerW);
        }

        sendsView.setBounds (area);
        sendsView.setScrollBarsShown (content > area.getHeight(), false, true, false);
        sendsPanel.setSize (juce::jmax (0, innerW), juce::jmax (content, area.getHeight()));
    }
}

void RackComponent::layoutSlots (juce::Rectangle<int> area)
{
    // the sends divider can take all the room: then only the sends are shown (or the view hides them)
    slotView.setVisible (showInserts && (vertical ? area.getHeight() : area.getWidth()) >= 12);

    if (! slotView.isVisible())
        return;

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

    // never smaller than readable: if they don't fit, the slots scroll (wheel / two fingers)
    const auto minPlugin = vertical ? 40.0f : 72.0f, minEmptySlot = vertical ? 20.0f : 26.0f;
    float total = 0.0f;

    for (int i = 0; i < slots.size(); ++i)
    {
        // a plugin slot made bigger keeps (at least) that factor of the readable minimum: with
        // little room (big sends / input / output) the slots scroll instead of squeezing it back
        lens[(size_t) i] = juce::jmax (lens[(size_t) i], rack.getPlugin (i) != nullptr ? minPlugin * juce::jmax (1.0f, rack.getSlot (i).uiSize)
                                                                                       : minEmptySlot);
        total += lens[(size_t) i];
    }

    const auto contentLength = juce::jmax ((int) length, juce::roundToInt (total));
    const bool scrolls = contentLength > (int) length;
    const auto sb = slotView.getScrollBarThickness();

    slotView.setBounds (area);
    slotView.setScrollBarsShown (vertical && scrolls, ! vertical && scrolls, vertical, ! vertical);

    const auto across = vertical ? area.getWidth() - (scrolls ? sb + 1 : 0) : area.getHeight() - (scrolls ? sb + 1 : 0);
    slotHolder.setSize (vertical ? across : contentLength, vertical ? contentLength : across);

    float pos = 0.0f;

    for (int i = 0; i < slots.size(); ++i)
    {
        const auto start = juce::roundToInt (pos);
        pos += lens[(size_t) i];
        const auto end = i == slots.size() - 1 ? contentLength : juce::roundToInt (pos);

        if (vertical)
            slots[i]->setBounds (0, start, across, end - start);
        else
            slots[i]->setBounds (start, 0, end - start, across);
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

    if (! folderArea.isEmpty() && folderArea.expanded (2).contains (e.getEventRelativeTo (this).getPosition()))
    {
        session.setFolderOpen (rack, ! rack.folderOpen);
        return;
    }

    if (rack.editGroup > 0 && groupArea.contains (e.getEventRelativeTo (this).getPosition()))
    {
        // click: suspend / resume the group (all its racks), like Pro Tools' group list
        session.setEditGroupActive (rack.editGroup, ! session.isEditGroupActive (rack.editGroup));
        if (auto* p = getParentComponent()) p->repaint();
        return;
    }

    if ((automixArea.contains (e.getEventRelativeTo (this).getPosition()) || automixMeterArea.contains (e.getEventRelativeTo (this).getPosition()))
        && rack.automixGroup.load() > 0)
    {
        if (! edit.isRackSelected (rack.getId()))
            edit.clickRack (rack.getId(), {});

        showAutomixMenu();
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

juce::PopupMenu RackComponent::editGroupMenu()
{
    // Pro Tools style edit groups: the selected racks (or this one) join a group; while the group is
    // active, gain (relative), mute/solo/bypass, plugin parameters, insert/remove/on-off follow.
    juce::Component::SafePointer<RackComponent> safe (this);
    juce::PopupMenu m;
    m.addSectionHeader ("Racks of a group follow each other");

    for (int g = 0; g <= Rack::maxEditGroups; ++g)
    {
        const auto n = g > 0 ? session.numRacksInEditGroup (g) : 0;
        juce::PopupMenu::Item item (g == 0 ? juce::String ("None")
                                           : "Group " + theme::groupLetter (g) + (n > 0 ? "  (" + juce::String (n) + " racks)" : juce::String()));
        item.setTicked (rack.editGroup == g);
        if (g > 0) item.setColour (theme::groupColour (g));
        item.setAction ([safe, g]
        {
            if (safe == nullptr) return;

            for (auto id : safe->edit.targetRacks())
                if (auto* r = safe->session.getEngine().findRack (id))
                    r->editGroup = g;

            if (g > 0 && ! safe->session.isEditGroupActive (g))
                safe->session.setEditGroupActive (g, true);

            safe->session.markDirty();
            if (safe->onResized) safe->onResized();   // the group badge appears / disappears
        });
        m.addItem (item);
    }

    if (const auto g = rack.editGroup; g > 0)
    {
        m.addSeparator();
        m.addItem (juce::PopupMenu::Item ("Group " + theme::groupLetter (g) + " active  (click the badge)")
                       .setTicked (session.isEditGroupActive (g))
                       .setAction ([safe, g]
                       {
                           if (safe == nullptr) return;
                           safe->session.setEditGroupActive (g, ! safe->session.isEditGroupActive (g));
                       }));
    }

    return m;
}

void RackComponent::showColourPicker()
{
    // free colour for the selected racks (or this one), applied while dragging
    class Picker final : public juce::ColourSelector, private juce::ChangeListener, private juce::Timer
    {
    public:
        Picker (Session& s, juce::Array<int> ids, juce::Colour start)
            : juce::ColourSelector (juce::ColourSelector::showColourspace | juce::ColourSelector::showSliders),
              session (s), racks (std::move (ids))
        {
            setCurrentColour (start, juce::dontSendNotification);
            setSize (260, 300);
            addChangeListener (this);
        }

        ~Picker() override { removeChangeListener (this); }   // no session access here: it may be closing

    private:
        void timerCallback() override { stopTimer(); session.markDirty(); }   // one undo step per pause
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            for (auto id : racks)
                if (auto* r = session.getEngine().findRack (id))
                    r->colour = getCurrentColour().withAlpha (1.0f);

            if (auto* top = getTopLevelComponent())
                top->repaint();

            startTimer (400);
        }

        Session& session;
        juce::Array<int> racks;
    };

    auto ids = edit.targetRacks();
    if (ids.isEmpty()) ids.add (rack.getId());

    auto* top = getTopLevelComponent();
    juce::CallOutBox::launchAsynchronously (std::make_unique<Picker> (session, ids, rack.colour),
                                            top->getLocalArea (this, menuButton.getBounds()), top);
}

juce::PopupMenu RackComponent::automixMenu()
{
    // applies to the selected racks (or this one)
    juce::Component::SafePointer<RackComponent> safe (this);
    auto forTargets = [safe] (auto fn)
    {
        return [safe, fn]
        {
            if (safe == nullptr)
                return;

            for (auto id : safe->edit.targetRacks())
                if (auto* r = safe->session.getEngine().findRack (id))
                    fn (*r);

            safe->session.markDirty();
            safe->resized();   // the automix bar appears / disappears
            safe->repaint();
            if (safe->onResized) safe->onResized();
        };
    };

    juce::PopupMenu m;
    m.addSectionHeader ("Gain-sharing automixer");

    for (int g = 0; g <= Rack::maxAutomixGroups; ++g)
        m.addItem (juce::PopupMenu::Item (g == 0 ? juce::String ("Off (not automixed)")
                                                 : "Group " + juce::String::charToString ((juce::juce_wchar) ('A' + g - 1)))
                       .setTicked (rack.automixGroup.load() == g)
                       .setAction (forTargets ([g] (Rack& r) { r.automixGroup = g; })));

    juce::PopupMenu weights;
    for (float w : { 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -10.0f, -15.0f, -20.0f })
        weights.addItem (juce::PopupMenu::Item ((w > 0 ? "+" : "") + juce::String (w, 0) + " dB" + (w == 0.0f ? "  (default)" : ""))
                             .setTicked (std::abs (rack.automixWeightDb.load() - w) < 0.05f)
                             .setAction (forTargets ([w] (Rack& r) { r.automixWeightDb = w; })));

    m.addSubMenu ("Weight", weights, rack.automixGroup.load() > 0);
    m.addSeparator();

    auto& engine = session.getEngine();
    juce::PopupMenu overlap;
    const char* overlapNames[] = { "Share (Dugan): -3 dB each", "Soft: -1.2 dB each", "Open: 0 dB each" };
    for (int o = 0; o < 3; ++o)
        overlap.addItem (juce::PopupMenu::Item (overlapNames[o])
                             .setTicked (engine.automixOverlap.load() == o)
                             .setAction ([safe, o]
                             {
                                 if (safe == nullptr) return;
                                 safe->session.getEngine().automixOverlap = o;
                                 safe->session.markDirty();
                             }));
    m.addSubMenu ("When several talk", overlap);

    m.addItem (juce::PopupMenu::Item ("Automixer enabled (all groups)")
                   .setTicked (engine.automixEnabled.load())
                   .setAction ([safe]
                   {
                       if (safe == nullptr) return;
                       auto& en = safe->session.getEngine();
                       en.automixEnabled = ! en.automixEnabled.load();
                       safe->session.markDirty();
                   }));
    return m;
}

void RackComponent::showAutomixMenu()
{
    automixMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                     .withTargetScreenArea (localAreaToGlobal (automixArea)));
}

juce::PopupMenu RackComponent::busMenu()
{
    juce::Component::SafePointer<RackComponent> safe (this);
    auto& engine = session.getEngine();
    juce::PopupMenu m;

    m.addItem ("Buses and sends in this session", true, engine.isRoutingEnabled(), [safe]
    {
        if (safe != nullptr)
            safe->session.setRoutingEnabled (! safe->session.getEngine().isRoutingEnabled());
    });
    m.addSeparator();
    m.addSectionHeader ("This rack is");

    for (int k = 0; k <= Rack::maxBuses; ++k)
    {
        // a letter used by another rack shows who has it
        juce::String owner;
        for (int i = 0; k > 0 && i < engine.getNumRacks(); ++i)
            if (auto& other = engine.getRack (i); &other != &rack && other.busIndex.load() == k)
                owner = other.name;

        const auto name = k == 0 ? juce::String ("A normal rack")
                                 : "Bus " + juce::String::charToString ((juce::juce_wchar) ('A' + k - 1))
                                       + (owner.isNotEmpty() ? "  (" + owner + ")" : juce::String());

        m.addItem (juce::PopupMenu::Item (name)
                       .setTicked (rack.busIndex.load() == k)
                       .setEnabled (owner.isEmpty())
                       .setColour (k > 0 ? theme::groupColour (k) : juce::Colour())
                       .setAction ([safe, k]
                       {
                           if (safe == nullptr) return;
                           auto& rc = *safe;
                           auto& eng = rc.session.getEngine();

                           if (k > 0 && ! eng.isRoutingEnabled())
                               eng.setRoutingEnabled (true);

                           if (eng.setRackBus (rc.rack, k))
                               rc.session.markDirty();
                       }));
    }

    return m;
}

juce::PopupMenu RackComponent::folderMenu()
{
    juce::Component::SafePointer<RackComponent> safe (this);
    auto act = [safe] (auto fn) { return [safe, fn] { if (safe != nullptr) fn (*safe); }; };
    auto& engine = session.getEngine();
    juce::PopupMenu m;

    m.addItem (juce::PopupMenu::Item ("Group into folder")
                   .setEnabled (engine.firstFreeBus() > 0)
                   .setAction (act ([] (RackComponent& rc)
                   {
                       auto ids = rc.edit.targetRacks();
                       if (ids.isEmpty()) ids.add (rc.rack.getId());
                       rc.session.groupIntoFolder (ids);
                   })));

    if (rack.isFolder)
    {
        m.addItem (rack.folderOpen ? "Close folder" : "Open folder", act ([] (RackComponent& rc)
        {
            rc.session.setFolderOpen (rc.rack, ! rc.rack.folderOpen);
        }));
        m.addItem ("Ungroup (keep the racks)", act ([] (RackComponent& rc) { rc.session.ungroupFolder (rc.rack); }));
    }

    if (rack.folderId >= 0)
        m.addItem ("Remove from folder", act ([] (RackComponent& rc)
        {
            auto ids = rc.edit.targetRacks();
            if (! ids.contains (rc.rack.getId())) ids = { rc.rack.getId() };
            for (auto id : ids)
                if (auto* r = rc.session.getEngine().findRack (id))
                    rc.session.removeFromFolder (*r);
        }));

    juce::PopupMenu into;
    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        auto& f = engine.getRack (i);
        if (! f.isFolder || &f == &rack)
            continue;

        const auto fid = f.getId();
        into.addItem (juce::PopupMenu::Item (juce::String (i + 1).paddedLeft ('0', 2) + "  " + f.name)
                          .setTicked (rack.folderId == fid)
                          .setColour (f.colour)
                          .setAction (act ([fid] (RackComponent& rc)
                          {
                              auto ids = rc.edit.targetRacks();
                              if (! ids.contains (rc.rack.getId())) ids = { rc.rack.getId() };
                              for (auto id : ids)
                                  if (auto* r = rc.session.getEngine().findRack (id))
                                      if (auto* folder = rc.session.getEngine().findRack (fid))
                                          rc.session.moveIntoFolder (*r, *folder);
                          })));
    }

    if (into.getNumItems() > 0)
        m.addSubMenu ("Move into folder", into);

    return m;
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
    juce::PopupMenu tempos;
    tempos.addSectionHeader ("Host tempo of this rack's plugins");
    for (int t = 0; t < session.getEngine().tempo.getNumTempos(); ++t)
    {
        auto& tempo = session.getEngine().tempo;
        const auto cfg = tempo.getConfig (t);
        tempos.addItem (juce::PopupMenu::Item (TempoEngine::shortName (t) + "  " + cfg.name + "  (" + juce::String (tempo.getBpm (t), 1) + " BPM)")
                            .setColour (tempoColour (t))
                            .setTicked (rack.tempoIndex.load() == t)
                            .setAction (act ([t] (RackComponent& rc)
                            {
                                for (auto id : rc.edit.targetRacks())
                                    if (auto* r = rc.session.getEngine().findRack (id))
                                        r->tempoIndex = t;
                                rc.session.markDirty();
                            })));
    }

    tempos.addSeparator();
    tempos.addItem ("New tempo for these racks", session.getEngine().tempo.getNumTempos() < TempoEngine::maxTempos, false,
                    act ([] (RackComponent& rc)
                    {
                        const auto t = rc.session.addTempo();
                        if (t <= 0) return;
                        for (auto id : rc.edit.targetRacks())
                            if (auto* r = rc.session.getEngine().findRack (id))
                                r->tempoIndex = t;
                        rc.session.markDirty();
                    }));

    m.addSubMenu ("Bus", busMenu());
    m.addSubMenu ("Rack View" + juce::String (rack.uiHideSections >= 0 || rack.uiSendsShown >= 0 ? "  (own)" : ""),
                  rackOwnViewMenu (session, edit, rack));
    m.addSubMenu ("Folder", folderMenu());
    m.addSubMenu ("Edit group", editGroupMenu());
    m.addSubMenu ("Latency group", groups);
    m.addSubMenu ("Automix", automixMenu());
    m.addSubMenu ("Tempo", tempos);

    juce::PopupMenu colours;
    const std::pair<const char*, juce::uint32> palette[] = {
        { "Blue", 0xff31c4f3 }, { "Green", 0xff7ed957 }, { "Orange", 0xffffb341 }, { "Red", 0xffff5f6d },
        { "Purple", 0xffb98cff }, { "Teal", 0xff45e0c0 }, { "Pink", 0xfff26fd1 }, { "Yellow", 0xffe8e35a },
        { "Indigo", 0xff6c7cff }, { "Lime", 0xffc0f05a }, { "Brown", 0xffc08a5a }, { "Grey", 0xff9aa3ad } };

    for (auto& [name, argb] : palette)
    {
        const juce::Colour c (argb);
        colours.addItem (juce::PopupMenu::Item (juce::String::repeatedString (juce::String::charToString (0x25A0), 4) + "  " + name)
                             .setColour (c)
                             .setTicked (rack.colour == c)
                             .setAction (act ([c] (RackComponent& rc)
                             {
                                 for (auto id : rc.edit.targetRacks())
                                     if (auto* r = rc.session.getEngine().findRack (id))
                                         r->colour = c;
                                 rc.session.markDirty();
                             })));
    }

    colours.addSeparator();
    colours.addItem ("Custom...", act ([] (RackComponent& rc) { rc.showColourPicker(); }));

    m.addSubMenu ("Colour", colours);
    m.addSeparator();
    m.addItem (menuItem ("Delete", "\xe2\x8c\xab", [&e] { e.deleteSelection(); }));

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton));
}

} // namespace stagerack
