#pragma once

#include <JuceHeader.h>

namespace stagerack
{

/** Floating window holding a plugin editor (or a generic editor if the plugin has none). */
class PluginWindow final : public juce::DocumentWindow,
                           private juce::Timer
{
public:
    PluginWindow (juce::AudioPluginInstance&, const juce::String& title, bool activate = true);
    ~PluginWindow() override;

    juce::AudioPluginInstance& getPlugin() noexcept { return plugin; }
    void closeButtonPressed() override;

    /** One window per plugin; toggles it if already open. */
    static void show (juce::AudioPluginInstance&, const juce::String& title);
    static void closeFor (juce::AudioPluginInstance*);
    /** Moves an open editor window from one instance to another (same place, no focus steal). */
    static void replace (juce::AudioPluginInstance* oldOne, juce::AudioPluginInstance* newOne);
    static void closeAll();

private:
    void timerCallback() override;       // refreshes the slot thumbnail while the window is open
    void captureThumbnail();

    juce::AudioPluginInstance& plugin;
    static juce::OwnedArray<PluginWindow>& windows();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

} // namespace stagerack
