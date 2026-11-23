#pragma once

#include "../Engine/Session.h"
#include "EditController.h"
#include "MidiTargets.h"
#include "ValueEntry.h"

namespace stagerack
{

/** The sends of one rack, one per bus it can send to (Ableton style): knobs when the rack is
    wide enough, bars when it is narrow. Drag = level (knobs: up/down, bars: left/right; Shift
    fine), double-click = off / 0 dB, right-click = pre/post fader. Alt = all selected racks
    (Alt+Shift relative), edit-group members follow relatively. The mouse wheel is left to the
    viewport around it (scrolls the sends that do not fit). Shown only while the session's buses
    & sends are on and there is a bus to send to. */
class SendsPanel final : public juce::Component,
                         public juce::SettableTooltipClient,
                         public MidiTargetProvider
{
public:
    void collectMidiTargets (std::vector<MidiTargetHit>& hits) override
    {
        for (int i = 0; i < (int) letters.size(); ++i)
            hits.push_back ({ cellBounds (i), MidiTarget::forRack (MidiTarget::rackSend, MidiTarget::fixedRack, rack.getId(), letters[(size_t) i]) });
    }
    SendsPanel (Session&, EditController&, Rack&);

    /** Buses changed (or appeared/disappeared): true if the number of sends changed. */
    bool update();
    int getNumSends() const noexcept                { return (int) letters.size(); }
    bool wantsToBeShown() const noexcept            { return ! letters.empty(); }

    /** Height all the sends need at this width (knobs or bars), given the room set with setFitHeight. */
    int getContentHeight (int width) const noexcept;
    /** Room the view gives the sends: when it is more than they need, knobs (and bars) grow to
        fill it (fewer columns, bigger knobs); 0 = compact. */
    void setFitHeight (int h) noexcept              { fitHeight = h; }
    /** Height of one row of sends at this width (the smallest useful view). */
    static int getRowHeight (int width) noexcept    { return cellHFor (width); }
    /** Send style, for the whole app (setting `sendStyle`, View → Sends as Knobs, right-click on a
        send): knobs (default, user choice) or lines (bus name + a bar with the value written, tall
        enough for a finger). Racks narrower than minKnobsWidth get one knob per row, with the level
        written inside and the bus name below. */
    inline static bool knobStyle = true;
    /** Called after the style is switched from a send's menu (MainComponent lays the racks out again). */
    inline static std::function<void()> onStyleChanged;
    static bool usesKnobs (int) noexcept            { return knobStyle; }
    static bool miniKnobs (int width) noexcept      { return knobStyle && width < minKnobsWidth; }
    static int cellWFor (int width) noexcept        { return miniKnobs (width) ? juce::jmax (1, width) : knobCellW; }
    static int cellHFor (int width) noexcept        { return usesKnobs (width) ? (miniKnobs (width) ? miniCellH : knobCellH) : stripH; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** "Sends shown" for this rack: as the session's view, or its own choice of buses (Alt: all
        selected racks). Used by the send's right-click menu and the rack menu. */
    static juce::PopupMenu sendsShownMenu (Session&, EditController&, Rack&);

    static constexpr float minDb = -60.0f, maxDb = 6.0f;
    static constexpr int stripH = 30, stripMaxH = 40;   // narrow racks: name line + bar
    static constexpr int barH = 15, knobCellW = 50, knobCellH = 48, minKnobsWidth = 100, miniCellH = 50;
    static float dbToPos (float db) noexcept;
    static float posToDb (float pos) noexcept;     // pos <= ~0 = off

private:
    int cellAt (juce::Point<int>) const;
    juce::Rectangle<int> cellBounds (int index) const;
    struct Grid { int cols, cellH; };
    Grid grid (int width) const noexcept;
    int numColumns() const noexcept                 { return grid (getWidth()).cols; }
    bool knobs() const noexcept                     { return usesKnobs (getWidth()); }
    void paintBar (juce::Graphics&, int index);
    void paintKnob (juce::Graphics&, int index);
    void setLevel (int letter, float db, const juce::ModifierKeys&, float relativeDelta = 0.0f);
    void showMenu (int index);

    Session& session;
    EditController& edit;
    Rack& rack;
    std::vector<int> letters;            // buses shown, in letter order
    std::vector<float> shown;            // last painted levels (repaint on change)
    int dragCell = -1, hoverCell = -1, fitHeight = 0;
    float dragStartPos = 0.0f, dragLastDb = 0.0f, dragPos = 0.0f;
    juce::Point<float> dragLast;
    ValueEntry entry;        // plain click = type the level
    bool altClick = false;   // Alt-click without moving = send back to its default (off)
    juce::ModifierKeys dragMods;

    JUCE_DECLARE_NON_COPYABLE (SendsPanel)
};

} // namespace stagerack
