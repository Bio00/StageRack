#pragma once

#include "../Engine/Session.h"

namespace stagerack
{

/** The automixer at work, like the console view of a Dugan: one strip per rack with its input
    level, the gain the automixer gives it right now, its weight, group and mute. Strips look
    racks up by id on every tick, so adding/removing racks or loading a session is safe. */
class AutomixerPanel final : public juce::Component,
                             private juce::Timer
{
public:
    explicit AutomixerPanel (Session&);
    ~AutomixerPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

    /** Rebuilds the strips if the racks changed, then updates the meters (also called by tests). */
    void tick();

    static juce::Colour groupColour (int group);

private:
    class Strip;

    void timerCallback() override   { tick(); }
    void rebuildStrips();

    Session& session;
    juce::TextButton enableButton { "AUTOMIX ON" };
    juce::Label infoLabel, overlapLabel;
    juce::ComboBox overlapBox;
    juce::Viewport viewport;
    juce::Component stripHolder;
    juce::OwnedArray<Strip> strips;
    std::vector<int> stripIds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomixerPanel)
};

class AutomixerWindow final : public juce::DocumentWindow
{
public:
    explicit AutomixerWindow (Session&);
    void closeButtonPressed() override { setVisible (false); }
};

} // namespace stagerack
