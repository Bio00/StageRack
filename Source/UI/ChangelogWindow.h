#pragma once

#include <JuceHeader.h>

namespace stagerack
{

/**
    A small window that shows release notes (the markdown of CHANGELOG.md / GitHub releases):
    "## " version headings, "### " sections, "- " bullets, **bold**, tables as plain rows.
    One window at a time: show() replaces its text and brings it to the front.
*/
class ChangelogWindow final : public juce::DocumentWindow
{
public:
    static void show (const juce::String& title, const juce::String& markdown);
    static void closeAll();

    /** Called when the window is closed (the update offer reopens itself). */
    static void setOnClose (std::function<void()>);

    void closeButtonPressed() override;

    /** Diagnostics (--render-changelog): the window content drawn off screen. */
    static juce::Image render (const juce::String& markdown, int width, int height);

private:
    class Content;
    ChangelogWindow();
    std::function<void()> onClose;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChangelogWindow)
};

} // namespace stagerack
