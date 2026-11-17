#include "EditGroupsWindow.h"
#include "Theme.h"

namespace stagerack
{

class EditGroupsPanel::Row final : public juce::Component
{
public:
    Row (EditGroupsPanel& p, int g) : panel (p), group (g)
    {
        activeButton.setClickingTogglesState (true);
        activeButton.setTooltip ("Active: the racks of this group follow each other. Off: suspended (each rack on its own), membership kept.");
        activeButton.onClick = [this]
        {
            panel.session.setEditGroupActive (group, activeButton.getToggleState());
            panel.changed();
        };
        addAndMakeVisible (activeButton);

        addButton.setTooltip ("Put the selected racks in this group");
        addButton.onClick = [this]
        {
            for (auto id : panel.edit.targetRacks())
                if (auto* r = panel.session.getEngine().findRack (id))
                    r->editGroup = group;

            if (! panel.session.isEditGroupActive (group))
                panel.session.setEditGroupActive (group, true);

            panel.session.markDirty();
            panel.changed();
        };
        addAndMakeVisible (addButton);

        removeButton.setTooltip ("Take the selected racks out of this group");
        removeButton.onClick = [this]
        {
            for (auto id : panel.edit.targetRacks())
                if (auto* r = panel.session.getEngine().findRack (id); r != nullptr && r->editGroup == group)
                    r->editGroup = 0;

            panel.session.markDirty();
            panel.changed();
        };
        addAndMakeVisible (removeButton);

        clearButton.setTooltip ("Empty this group (its racks leave it)");
        clearButton.onClick = [this]
        {
            auto& e = panel.session.getEngine();
            for (int i = 0; i < e.getNumRacks(); ++i)
                if (e.getRack (i).editGroup == group)
                    e.getRack (i).editGroup = 0;

            panel.session.markDirty();
            panel.changed();
        };
        addAndMakeVisible (clearButton);
        lookAndFeelChanged();
    }

    void lookAndFeelChanged() override
    {
        activeButton.setColour (juce::TextButton::buttonOnColourId, theme::groupColour (group));
        activeButton.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff101214));
    }

    void update()
    {
        auto& e = panel.session.getEngine();
        juce::StringArray names;
        for (int i = 0; i < e.getNumRacks(); ++i)
            if (e.getRack (i).editGroup == group)
                names.add (e.getRack (i).name);

        const auto selected = panel.edit.targetRacks();
        int selectedIn = 0;
        for (auto id : selected)
            if (auto* r = e.findRack (id); r != nullptr && r->editGroup == group)
                ++selectedIn;

        const bool active = panel.session.isEditGroupActive (group);
        activeButton.setToggleState (active, juce::dontSendNotification);
        activeButton.setButtonText (active ? "ACTIVE" : "OFF");
        activeButton.setEnabled (! names.isEmpty());
        addButton.setEnabled (! selected.isEmpty() && selectedIn < selected.size());
        removeButton.setEnabled (selectedIn > 0);
        clearButton.setEnabled (! names.isEmpty());

        if (names != members || active != wasActive)
        {
            members = names;
            wasActive = active;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.0f, 2.0f);
        const auto c = theme::groupColour (group);
        const bool used = ! members.isEmpty();
        const bool on = used && wasActive;

        g.setColour (theme::panel);
        g.fillRoundedRectangle (b, 6.0f);

        auto pill = pillArea.toFloat();
        g.setColour (on ? c : (used ? c.withAlpha (0.15f) : theme::meterBg));
        g.fillRoundedRectangle (pill, 4.0f);
        g.setColour (used ? c : theme::outline);
        if (! on) g.drawRoundedRectangle (pill.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (on ? juce::Colour (0xff101214) : (used ? c : theme::textDim));
        g.setFont (theme::font (16.0f, true));
        g.drawText (theme::groupLetter (group), pill, juce::Justification::centred, false);

        g.setFont (theme::font (12.0f, used));
        g.setColour (used ? theme::text : theme::textDim);
        g.drawFittedText (used ? juce::String (members.size()) + (members.size() == 1 ? " rack:  " : " racks:  ") + members.joinIntoString (", ")
                               : juce::String ("empty"),
                          membersArea, juce::Justification::centredLeft, 2, 0.8f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 6);
        pillArea = r.removeFromLeft (34).withSizeKeepingCentre (34, 28);
        r.removeFromLeft (10);
        activeButton.setBounds (r.removeFromLeft (74).withSizeKeepingCentre (74, 26));
        r.removeFromLeft (12);
        clearButton.setBounds (r.removeFromRight (62).withSizeKeepingCentre (62, 26));
        r.removeFromRight (6);
        removeButton.setBounds (r.removeFromRight (100).withSizeKeepingCentre (100, 26));
        r.removeFromRight (6);
        addButton.setBounds (r.removeFromRight (100).withSizeKeepingCentre (100, 26));
        r.removeFromRight (10);
        membersArea = r;
    }

private:
    EditGroupsPanel& panel;
    const int group;
    juce::TextButton activeButton, addButton { "+ SELECTED" }, removeButton { "- SELECTED" }, clearButton { "CLEAR" };
    juce::Rectangle<int> pillArea, membersArea;
    juce::StringArray members;
    bool wasActive = true;
};

//==============================================================================
EditGroupsPanel::EditGroupsPanel (Session& s, EditController& e) : session (s), edit (e)
{
    allOnButton.setTooltip ("Activate every group that has racks (Cmd+Shift+G toggles all)");
    allOnButton.onClick = [this]
    {
        for (int g = 1; g <= Rack::maxEditGroups; ++g)
            if (session.numRacksInEditGroup (g) > 0 && ! session.isEditGroupActive (g))
                session.setEditGroupActive (g, true);
        changed();
    };
    allOffButton.setTooltip ("Suspend every group (racks keep their group)");
    allOffButton.onClick = [this]
    {
        for (int g = 1; g <= Rack::maxEditGroups; ++g)
            if (session.isEditGroupActive (g))
                session.setEditGroupActive (g, false);
        changed();
    };
    addAndMakeVisible (allOnButton);
    addAndMakeVisible (allOffButton);

    helpLabel.setText ("Racks of an ACTIVE group follow each other's gain (relative), mute / solo / bypass and plugins "
                       "(insert, remove, on/off, parameters). Select racks (click, Cmd/Shift-click) and press + SELECTED, "
                       "or use the rack menu > Edit group. Routing never follows a group.",
                       juce::dontSendNotification);
    helpLabel.setFont (theme::font (11.0f));
    helpLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (helpLabel);

    for (int g = 1; g <= Rack::maxEditGroups; ++g)
        addAndMakeVisible (rows.add (new Row (*this, g)));

    lookAndFeelChanged();
    setSize (640, 44 + Rack::maxEditGroups * 46 + 54);
    tick();
    startTimerHz (10);
}

EditGroupsPanel::~EditGroupsPanel() = default;

void EditGroupsPanel::changed()
{
    tick();
    if (onChanged) onChanged();
}

void EditGroupsPanel::tick()
{
    for (auto* r : rows)
        r->update();
}

void EditGroupsPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    g.setColour (theme::text);
    g.setFont (theme::font (14.0f, true));
    g.drawText ("EDIT GROUPS", getLocalBounds().reduced (14, 10).removeFromTop (26), juce::Justification::centredLeft, false);
}

void EditGroupsPanel::lookAndFeelChanged()
{
    helpLabel.setColour (juce::Label::textColourId, theme::textDim);
}

void EditGroupsPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto header = r.removeFromTop (26);
    allOffButton.setBounds (header.removeFromRight (80));
    header.removeFromRight (6);
    allOnButton.setBounds (header.removeFromRight (80));
    r.removeFromTop (8);
    helpLabel.setBounds (r.removeFromBottom (44));

    for (auto* row : rows)
        row->setBounds (r.removeFromTop (46));
}

//==============================================================================
EditGroupsWindow::EditGroupsWindow (Session& s, EditController& e)
    : juce::DocumentWindow ("Edit Groups", theme::bg, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new EditGroupsPanel (s, e), true);
    setResizable (true, false);
    setResizeLimits (520, 300, 2000, 1200);
    centreWithSize (getWidth(), getHeight());
}

} // namespace stagerack
