#pragma once

#include "../Engine/Session.h"
#include "../Engine/MidiControl.h"
#include "MidiTargets.h"
#include <set>

namespace stagerack
{

/**
    MIDI map mode (Ableton style): lies over the main window and tints every control that can
    be mapped; mapped ones show their controller control ("Fader 3"). Click one, move a control
    on the controller: mapped. Touching a parameter in a plugin's own window selects it too.
    Clicks elsewhere go through (scrolling, opening plugin windows still work).
    Delete / Backspace removes the selected control's mappings, Esc leaves map mode.
*/
class MidiMapOverlay final : public juce::Component,
                             private juce::Timer
{
public:
    MidiMapOverlay (juce::Component& root, Session&, MidiControl&);

    std::function<void()> onOpenWindow, onDone;
    /** Map mode started / ended (MainComponent): starts the watch of the plugin windows. */
    void setActive (bool);

    void paint (juce::Graphics&) override;
    void resized() override;
    bool hitTest (int x, int y) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    /** For tests and renders: the targets found on screen now. */

private:
    void timerCallback() override;
    void collect();
    void collectFrom (juce::Component&, std::vector<MidiTargetHit>&);
    void watchPluginParameters();
    MidiTarget asLearnTarget (const MidiTarget& fixed) const;
    bool isLearnTarget (const MidiTarget& fixed) const;
    juce::String labelFor (const MidiTarget& fixed) const;
    std::vector<int> mappingsShownAt (const MidiTarget& fixed) const;
    void showMenu (const MidiTarget& fixed);
    void updateScopeButtons();
    void updateHint();

    juce::Component& root;
    Session& session;
    MidiControl& midi;

    struct Shown { juce::Rectangle<int> area; MidiTarget target; };
    std::vector<Shown> shown;

    juce::Rectangle<int> bandArea;
    bool bandAtTop = false;
    juce::Label hint;
    juce::TextButton scopeRack { "This rack" }, scopeBank { "Bank strip" }, scopeSelected { "Selected rack" };
    juce::TextButton windowButton { "MIDI window" }, doneButton { "Done" };

    // plugin windows: a parameter moved there becomes the selected control
    struct Snapshot { std::vector<float> values; std::vector<bool> moving; };
    std::map<const juce::AudioPluginInstance*, Snapshot> snapshots;
    int watchTicks = 0;
    juce::uint32 midiEventsAtLastTick = 0;
    juce::String notice;
    juce::uint32 noticeUntil = 0;

    JUCE_DECLARE_NON_COPYABLE (MidiMapOverlay)
};

} // namespace stagerack
