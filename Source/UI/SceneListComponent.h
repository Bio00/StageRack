#pragma once

#include "../Engine/Session.h"

namespace stagerack
{

/** Left sidebar: the scene (snapshot) list. Click = recall. */
class SceneListComponent final : public juce::Component,
                                 private juce::ListBoxModel
{
public:
    explicit SceneListComponent (Session&);

    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>&) override;

    void renameScene (int row);

    Session& session;
    juce::ListBox list;
    juce::TextButton addButton { "+ NEW" }, storeButton { "STORE" }, renameButton { "RENAME" }, deleteButton { "DELETE" };
};

} // namespace stagerack
