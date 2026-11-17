#pragma once

#include "../Engine/Session.h"
#include "EditController.h"

namespace stagerack
{

/** Edit groups A-H (Pro Tools style) in a window of their own: which racks are in each group,
    active / suspended, add the selected racks, clear. Racks of an active group follow each
    other's gain (relative), mute/solo/bypass and plugins. */
class EditGroupsPanel final : public juce::Component,
                              private juce::Timer
{
public:
    EditGroupsPanel (Session&, EditController&);
    ~EditGroupsPanel() override;

    /** Called after a change that moves rack badges (MainComponent re-lays out the racks). */
    std::function<void()> onChanged;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void tick();

private:
    class Row;
    void timerCallback() override   { tick(); }
    void changed();

    Session& session;
    EditController& edit;
    juce::TextButton allOnButton { "ALL ON" }, allOffButton { "ALL OFF" };
    juce::Label helpLabel;
    juce::OwnedArray<Row> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditGroupsPanel)
};

class EditGroupsWindow final : public juce::DocumentWindow
{
public:
    EditGroupsWindow (Session&, EditController&);
    EditGroupsPanel& getPanel()     { return *static_cast<EditGroupsPanel*> (getContentComponent()); }
    void closeButtonPressed() override { setVisible (false); }
};

} // namespace stagerack
