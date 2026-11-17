#pragma once

#include "../Engine/MidiMap.h"

namespace stagerack
{

/** A control on screen that MIDI map mode can map: its area (in the component's own
    coordinates) and what it drives. */
struct MidiTargetHit
{
    juce::Rectangle<int> bounds;
    MidiTarget target;
};

/** Implemented by components holding mappable controls (racks, slots, parameter rows, sends,
    top bar): map mode finds them by walking the window's components. */
struct MidiTargetProvider
{
    virtual ~MidiTargetProvider() = default;
    virtual void collectMidiTargets (std::vector<MidiTargetHit>&) = 0;
};

} // namespace stagerack
