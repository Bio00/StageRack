#include "PluginWindow.h"
#include "Theme.h"
#include "PluginThumbnail.h"

namespace stagerack
{

juce::OwnedArray<PluginWindow>& PluginWindow::windows()
{
    static juce::OwnedArray<PluginWindow> list;
    return list;
}

PluginWindow::PluginWindow (juce::AudioPluginInstance& p, const juce::String& title, bool activate)
    : juce::DocumentWindow (title, theme::bg, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
      plugin (p)
{
    setUsingNativeTitleBar (true);

    juce::AudioProcessorEditor* editor = nullptr;

    if (plugin.hasEditor())
        editor = plugin.createEditorAndMakeActive();

    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor (plugin);

    setContentOwned (editor, true);
    setResizable (editor->isResizable(), false);

    const auto mouse = juce::Desktop::getMousePosition();
    setTopLeftPosition (mouse.x - getWidth() / 2, juce::jmax (40, mouse.y - 60));
    setVisible (true);
    toFront (activate);
    startTimer (800);   // first picture once the editor had time to draw, then every 2 s
}

PluginWindow::~PluginWindow()
{
    stopTimer();
    clearContentComponent();
}

void PluginWindow::captureThumbnail()
{
    if (auto* content = getContentComponent(); content != nullptr && isVisible() && ! isMinimised())
        PluginThumbnails::get().captureFrom (&plugin, *content);
}

void PluginWindow::timerCallback()
{
    captureThumbnail();
    startTimer (2000);
}

void PluginWindow::closeButtonPressed()
{
    captureThumbnail();   // the mini view keeps the latest settings
    windows().removeObject (this);
}

void PluginWindow::show (juce::AudioPluginInstance& p, const juce::String& title)
{
    for (auto* w : windows())
    {
        if (&w->plugin == &p)
        {
            w->toFront (true);
            return;
        }
    }

    // an invisible editor may exist for a pending mini-view capture: a plugin has one editor at a time
    PluginThumbnails::get().cancelOffscreen (&p);
    windows().add (new PluginWindow (p, title));
}

void PluginWindow::closeFor (juce::AudioPluginInstance* p)
{
    // called right before the plugin is deleted
    for (int i = windows().size(); --i >= 0;)
        if (&windows()[i]->plugin == p)
            windows().remove (i);

    PluginThumbnails::get().forget (p);
}

void PluginWindow::replace (juce::AudioPluginInstance* oldOne, juce::AudioPluginInstance* newOne)
{
    if (oldOne == nullptr || newOne == nullptr)
        return;

    for (int i = windows().size(); --i >= 0;)
    {
        if (&windows()[i]->plugin != oldOne)
            continue;

        const auto pos = windows()[i]->getPosition();
        const auto title = windows()[i]->getName();
        const bool minimised = windows()[i]->isMinimised();
        windows().remove (i);

        PluginThumbnails::get().cancelOffscreen (newOne);
        auto* w = windows().add (new PluginWindow (*newOne, title, false));
        w->setTopLeftPosition (pos);
        w->setMinimised (minimised);
        return;
    }
}

void PluginWindow::closeAll()
{
    windows().clear();
    PluginThumbnails::get().reset();
}

} // namespace stagerack
