#pragma once

#include <JuceHeader.h>

namespace stagerack
{

/**
    Slider with the editing rules of every StageRack control:
    - Alt-click (press and release without moving) = back to the default value
      (setDoubleClickReturnValue). Alt-drag still drags, so Alt keeps meaning "all the selected
      racks" while dragging; a reset with Alt on a selected rack resets them all.
    - Cmd (or Shift) held = fine: the drag moves 5x slower. It can be pressed or released in the
      middle of a drag without the value jumping.
    Works with relative drags (rotary, faders that don't snap to the mouse).
*/
class ParamSlider : public juce::Slider
{
public:
    static constexpr float fineFactor = 0.2f;

    static bool isFine (const juce::ModifierKeys& m) { return m.isCommandDown() || m.isShiftDown(); }

    ParamSlider()
    {
        setVelocityModeParameters (1.0, 1, 0.0, false);   // no modifier swaps the drag mode
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        altClick = e.mods.isAltDown() && ! e.mods.isPopupMenu() && isDoubleClickReturnEnabled() && isEnabled();
        lastRaw = virtualPos = e.position;
        juce::Slider::mouseDown (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() > 2)
            altClick = false;

        // accumulate the movement, scaled while Cmd/Shift is held
        const auto k = isFine (e.mods) ? fineFactor : 1.0f;
        virtualPos += (e.position - lastRaw) * k;
        lastRaw = e.position;
        juce::Slider::mouseDrag (e.withNewPosition (virtualPos));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // reset before the base class ends the drag gesture (onDragEnd = undo step / dirty)
        if (altClick && e.getDistanceFromDragStart() <= 2)
            setValue (getDoubleClickReturnValue(), juce::sendNotificationSync);

        altClick = false;
        juce::Slider::mouseUp (e.withNewPosition (virtualPos));
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (isFine (e.mods))
        {
            auto fine = w;
            fine.deltaX *= fineFactor;
            fine.deltaY *= fineFactor;
            juce::Slider::mouseWheelMove (e, fine);
            return;
        }

        juce::Slider::mouseWheelMove (e, w);
    }

private:
    bool altClick = false;
    juce::Point<float> lastRaw, virtualPos;
};

} // namespace stagerack
