#include "SceneListComponent.h"
#include "Theme.h"

namespace stagerack
{

SceneListComponent::SceneListComponent (Session& s) : session (s)
{
    list.setModel (this);
    list.setRowHeight (34);
    addAndMakeVisible (list);

    addButton.setTooltip ("Create a new scene from the current settings");
    addButton.onClick = [this]
    {
        session.addScene ("Scene " + juce::String (session.getNumScenes() + 1));
    };

    storeButton.setTooltip ("Overwrite the selected scene with the current settings");
    storeButton.onClick = [this]
    {
        const auto row = list.getSelectedRow() >= 0 ? list.getSelectedRow() : session.getCurrentScene();
        session.storeScene (row);
    };

    renameButton.onClick = [this] { renameScene (list.getSelectedRow()); };

    deleteButton.onClick = [this]
    {
        const auto row = list.getSelectedRow();

        if (row >= 0)
            session.deleteScene (row);
    };

    for (auto* b : { &addButton, &storeButton, &renameButton, &deleteButton })
        addAndMakeVisible (*b);

    lookAndFeelChanged();
}

void SceneListComponent::lookAndFeelChanged()
{
    list.setColour (juce::ListBox::backgroundColourId, theme::panel);
    addButton.setColour (juce::TextButton::buttonColourId, theme::accent.withAlpha (0.25f));
    deleteButton.setColour (juce::TextButton::textColourOffId, theme::red);
}

void SceneListComponent::refresh()
{
    list.updateContent();

    if (session.getCurrentScene() >= 0)
    {
        list.selectRow (session.getCurrentScene(), true, true);
        list.scrollToEnsureRowIsOnscreen (session.getCurrentScene());
    }
    else
    {
        list.deselectAllRows();
    }

    list.repaint();
}

void SceneListComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);

    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f, true));
    g.drawText ("SCENES", 12, 8, getWidth() - 24, 18, juce::Justification::centredLeft);

    g.setFont (theme::font (11.0f));
    g.drawText (juce::String (session.getNumScenes()), 12, 8, getWidth() - 24, 18, juce::Justification::centredRight);

    g.setColour (theme::outline);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
}

void SceneListComponent::resized()
{
    auto r = getLocalBounds().reduced (8).withTrimmedTop (24);
    auto buttons = r.removeFromBottom (64);

    auto row1 = buttons.removeFromTop (30);
    buttons.removeFromTop (4);
    auto row2 = buttons;

    addButton.setBounds (row1.removeFromLeft (row1.getWidth() / 2).reduced (2, 0));
    storeButton.setBounds (row1.reduced (2, 0));
    renameButton.setBounds (row2.removeFromLeft (row2.getWidth() / 2).reduced (2, 0));
    deleteButton.setBounds (row2.reduced (2, 0));

    r.removeFromBottom (8);
    list.setBounds (r);
}

int SceneListComponent::getNumRows()
{
    return session.getNumScenes();
}

void SceneListComponent::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, session.getNumScenes()))
        return;

    const bool current = row == session.getCurrentScene();
    auto r = juce::Rectangle<int> (0, 0, w, h).reduced (2, 2).toFloat();

    g.setColour (current ? theme::accent.withAlpha (0.22f) : (selected ? theme::slotHover : theme::panelLight));
    g.fillRoundedRectangle (r, 4.0f);

    if (current)
    {
        g.setColour (theme::accent);
        g.fillRoundedRectangle (r.removeFromLeft (4.0f), 2.0f);
    }

    g.setColour (current ? theme::accent : theme::textDim);
    g.setFont (theme::font (13.0f, true));
    g.drawText (juce::String (row + 1).paddedLeft ('0', 2), 12, 0, 28, h, juce::Justification::centredLeft);

    g.setColour (theme::text);
    g.setFont (theme::font (13.5f, current));
    g.drawText (session.getScene (row).name, 42, 0, w - 50, h, juce::Justification::centredLeft, true);
}

void SceneListComponent::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addItem ("Recall", [this, row] { session.recallScene (row); });
        m.addItem ("Store current settings here", [this, row] { session.storeScene (row); });
        m.addItem ("Rename", [this, row] { renameScene (row); });
        m.addItem ("Move up", row > 0, false, [this, row] { session.moveScene (row, row - 1); });
        m.addItem ("Move down", row < session.getNumScenes() - 1, false, [this, row] { session.moveScene (row, row + 1); });
        m.addSeparator();
        m.addItem ("Delete", [this, row] { session.deleteScene (row); });
        m.showMenuAsync ({});
        return;
    }

    session.recallScene (row);
}

void SceneListComponent::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    renameScene (row);
}

juce::var SceneListComponent::getDragSourceDescription (const juce::SparseSet<int>&)
{
    return {};
}

void SceneListComponent::renameScene (int row)
{
    if (! juce::isPositiveAndBelow (row, session.getNumScenes()))
        return;

    auto* w = new juce::AlertWindow ("Rename scene", {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", session.getScene (row).name);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, row, w] (int result)
    {
        if (result == 1)
            session.renameScene (row, w->getTextEditorContents ("name"));
    }), true);
}

} // namespace stagerack
