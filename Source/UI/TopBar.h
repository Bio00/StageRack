#pragma once

#include "../Engine/Session.h"

namespace stagerack
{

/** Top bar, in sections by function (left to right):
    session (logo, file, NEW/OPEN/SAVE) | scenes | live (MUTE ALL, automixer, edit groups) |
    audio engine (DSP per core, device, PLUGINS/SETTINGS) | view (layout, theme).
    Every section has a minimum width: all of it stays visible down to the minimum window width. */
class TopBar final : public juce::Component,
                     public juce::SettableTooltipClient
{
public:
    explicit TopBar (Session&);

    /** Device problem to show instead of the device info ("Waiting for Dante..."), empty if none. */
    std::function<juce::String()> deviceStatus;
    std::function<void()> onNew, onOpen, onSave, onPlugins, onSettings, onToggleLayout, onAutomixView, onToggleTheme;
    void setVerticalLayout (bool);

    void refresh();
    void updateMeters();

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    static constexpr int preferredHeight = 76;

private:
    Session& session;

    juce::TextButton prevButton { juce::String::charToString (0x25C0) }, nextButton { juce::String::charToString (0x25B6) };
    juce::TextButton storeButton { "STORE" };
    juce::TextButton newButton { "NEW" }, openButton { "OPEN" }, saveButton { "SAVE" };
    juce::TextButton pluginsButton { "PLUGINS" }, settingsButton { "SETTINGS" }, layoutButton { "VERTICAL" }, themeButton { "LIGHT" };
    juce::TextButton automixButton { "AUTOMIX" }, automixViewButton { "AM METERS" };
    juce::TextButton masterMuteButton { "MUTE ALL" };
    int blink = 0;

    juce::Rectangle<int> logoArea, sceneArea, cpuArea, deviceArea, groupsArea;
    juce::Array<int> separators;   // x of the lines between sections
    juce::Rectangle<float> groupPill (int group) const;
    std::vector<float> threadLoads;
    float totalLoad = 0.0f;
};

} // namespace stagerack
