#pragma once

#include "../Plugins/PluginManager.h"

namespace stagerack
{

/** Pro Tools style plugin picker shown in a call-out box: a sidebar filters by category and
    manufacturer, a coloured dot shows each plugin's latency (green 0, yellow < 5 ms, grey >= 5 ms). */
class PluginBrowser final : public juce::Component,
                            private juce::Timer
{
public:
    PluginBrowser (PluginManager&, std::function<void (const juce::PluginDescription&)> onChoose);
    ~PluginBrowser() override;

    /** Category used in the sidebar (from the plugin's own category, or guessed from its name). */
    static juce::String categoryOf (const juce::PluginDescription&);

    void visibilityChanged() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class Sidebar;
    class PluginList;

    void timerCallback() override;
    void filter();
    void choose (int row);
    void updateMeasureButton();

    PluginManager& manager;
    std::function<void (const juce::PluginDescription&)> chosen;
    juce::Array<juce::PluginDescription> all, filtered;
    juce::StringArray categories;                     // per entry of `all`
    juce::TextEditor search;
    juce::ComboBox format;
    juce::TextButton measureButton;
    std::unique_ptr<Sidebar> sidebar;
    std::unique_ptr<PluginList> list;
    juce::Rectangle<int> legendArea;
    bool wasProbing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginBrowser)
};

} // namespace stagerack
