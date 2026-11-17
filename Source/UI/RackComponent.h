#pragma once

#include "../Engine/Session.h"
#include "EditController.h"
#include "SendsPanel.h"
#include "ParamSlider.h"

namespace stagerack
{

class SlotComponent;

/** ComboBox whose popup is built by its owner (the OUT selector: ticks on every destination,
    Ctrl-click / "Additional outputs" adds more). */
struct OwnPopupComboBox final : public juce::ComboBox
{
    std::function<void()> onPopup;
    void showPopup() override { if (onPopup) onPopup(); else juce::ComboBox::showPopup(); }
};

/** One rack: name, I/O routing, 8 plugin slots, meters, gain, mute/solo/bypass, per-core stats.
    Click/cmd/shift-click selects it, drag reorders it (alt-drag duplicates), the edge handle resizes it. */
class RackComponent final : public juce::Component,
                            public juce::DragAndDropTarget
{
public:
    RackComponent (Session&, EditController&, Rack&, int index);
    ~RackComponent() override;

    Rack& getRack() noexcept        { return rack; }

    void refresh();                 // structure/selection changed
    void updateMeters();            // ~30 Hz from the main timer
    void setVertical (bool shouldBeVertical);

    /** Height (horizontal layout) or width (vertical layout) for the rack's size factor. */
    int getPreferredLength() const;

    /** Called while the edge handle is dragged, so the owner can re-layout. */
    std::function<void()> onResized;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    static constexpr int baseHeight = 112, baseWidth = 190;   // tall enough for 3 parameter rows per slot

private:
    class ResizeHandle;
    class SplitHandle;

    void showRackMenu();
    juce::PopupMenu automixMenu();
    juce::PopupMenu editGroupMenu();
    void showColourPicker();
    bool isNarrow() const noexcept   { return vertical && getWidth() < 150; }
    bool isTiny() const noexcept     { return vertical && getWidth() < 96; }   // minimum width: stacked controls
    void showAutomixMenu();
    void fillRoutingBoxes();
    void layoutSlots (juce::Rectangle<int>);
    bool dropsAfter (juce::Point<int>) const;

    Session& session;
    EditController& edit;
    Rack& rack;
    const int index;

    juce::Label nameLabel, infoLabel;
    juce::ComboBox inputBox;
    OwnPopupComboBox outputBox;
    void showOutputMenu();
    void toggleExtraOutput (int code, const juce::ModifierKeys&);
    juce::String outputName (int code) const;
    juce::OwnedArray<SlotComponent> slots;
    ParamSlider gain, inputGain;   // output fader (after the plugins), input trim (before)
    juce::TextButton muteButton { "M" }, soloButton { "S" }, bypassButton { "B" };
    juce::TextButton menuButton { juce::String::fromUTF8 ("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2") };
    std::unique_ptr<ResizeHandle> resizeHandle;
    SendsPanel sendsPanel;
    juce::Viewport slotView, sendsView;      // plugins and sends scroll when they don't fit
    juce::Component slotHolder;
    std::unique_ptr<SplitHandle> splitHandle, inputHandle, outputHandle;   // sends | plugins, input | plugins, sends | output
    int inputLength = 0, outputLength = 0;    // the sections' heights now (the dividers start from these)
    juce::Rectangle<int> inScaleArea, outScaleArea;
    static void setFaderStyle (juce::Slider&, bool fader);
    juce::PopupMenu folderMenu();
    void layoutSends (juce::Rectangle<int>& area);   // takes the sends' room (and the divider) from the slots' area
    juce::PopupMenu busMenu();

    juce::Rectangle<int> inMeterArea, outMeterArea, indexArea, groupArea, automixArea, automixMeterArea, folderArea;

    // Status badges (CPU spikes, latency group, tempo...): all shown side by side, short forms
    // when the room is tight. Rebuilt on every meter tick.
    struct Badge { juce::String text, shortText, tip; juce::Colour colour; };
    std::vector<Badge> badges;
    juce::Rectangle<int> badgeArea;
    void updateBadges();
    void paintBadges (juce::Graphics&);
    Session::RackView laidOutView;  // the view resized() last used
    bool showInserts = true;        // the session's view shows the plugin slots
    bool tinyRouting = false;       // thinnest racks: bus names left out, "▶ B"
    bool compactRouting = false;    // short IN/OUT texts ("IN 1-2") on narrow racks
    float inLevel[2] {}, outLevel[2] {}, peakLoad = 0.0f, automixDb = 0.0f;
    bool vertical = false, gainDragging = false, inGainDragging = false;
    juce::ModifierKeys gainDragMods, inGainDragMods;
    int dropIndicator = 0;          // 0 = none, -1 = before this rack, 1 = after

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackComponent)
};

} // namespace stagerack
