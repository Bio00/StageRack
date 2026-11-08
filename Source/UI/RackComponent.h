#pragma once

#include "../Engine/Session.h"
#include "EditController.h"

namespace stagerack
{

class SlotComponent;

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

    void showRackMenu();
    void fillRoutingBoxes();
    void layoutSlots (juce::Rectangle<int>);
    bool dropsAfter (juce::Point<int>) const;

    Session& session;
    EditController& edit;
    Rack& rack;
    const int index;

    juce::Label nameLabel, statusLabel, infoLabel;
    juce::ComboBox inputBox, outputBox;
    juce::OwnedArray<SlotComponent> slots;
    juce::Slider gain;
    juce::TextButton muteButton { "M" }, soloButton { "S" }, bypassButton { "B" };
    juce::TextButton menuButton { juce::String::fromUTF8 ("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2") };
    std::unique_ptr<ResizeHandle> resizeHandle;

    juce::Rectangle<int> meterArea, indexArea;
    float inLevel[2] {}, outLevel[2] {}, peakLoad = 0.0f;
    bool vertical = false, gainDragging = false;
    juce::ModifierKeys gainDragMods;
    int dropIndicator = 0;          // 0 = none, -1 = before this rack, 1 = after

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackComponent)
};

} // namespace stagerack
