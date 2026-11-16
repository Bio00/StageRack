#pragma once

#include "../Engine/Session.h"

namespace stagerack
{

/** Top bar: logo, big current-scene display with prev/next, per-core DSP meters, device info. */
class TopBar final : public juce::Component
{
public:
    explicit TopBar (Session&);

    /** Device problem to show instead of the device info ("Waiting for Dante..."), empty if none. */
    std::function<juce::String()> deviceStatus;
    std::function<void()> onNew, onOpen, onSave, onPlugins, onSettings, onToggleLayout, onAutomixView;
    void setVerticalLayout (bool);

    void refresh();
    void updateMeters();

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

    static constexpr int preferredHeight = 76;

private:
    Session& session;

    juce::TextButton prevButton { juce::String::charToString (0x25C0) }, nextButton { juce::String::charToString (0x25B6) };
    juce::TextButton storeButton { "STORE" };
    juce::TextButton newButton { "NEW" }, openButton { "OPEN" }, saveButton { "SAVE" };
    juce::TextButton pluginsButton { "PLUGINS" }, settingsButton { "SETTINGS" }, layoutButton { "VERTICAL" };
    juce::TextButton automixButton { "AUTOMIX" }, automixViewButton { "AM METERS" };
    juce::TextButton masterMuteButton { "MUTE ALL" };
    int blink = 0;

    juce::Rectangle<int> sceneArea, cpuArea, deviceArea;
    std::vector<float> threadLoads;
    float totalLoad = 0.0f;
};

} // namespace stagerack
