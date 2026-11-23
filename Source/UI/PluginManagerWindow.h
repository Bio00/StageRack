#pragma once

#include "../Engine/AudioEngine.h"
#include "../Plugins/PluginManager.h"
#include "../Engine/Session.h"

namespace stagerack
{

/** Plugin list + scanning (out-of-process, multithreaded). */
class PluginManagerWindow final : public juce::DocumentWindow
{
public:
    explicit PluginManagerWindow (PluginManager&);
    void closeButtonPressed() override { setVisible (false); }
};

/** Audio/MIDI device, DSP thread count and MIDI scene control. */
class SettingsWindow final : public juce::DocumentWindow
{
public:
    SettingsWindow (AudioEngine&, PluginManager&, Session&, std::function<void (bool light)> onThemeChanged,
                    std::function<void()> onCheckUpdates);
    void closeButtonPressed() override { setVisible (false); }
    void showPage (int page);   // 0 audio, 1 performance, 2 MIDI, 3 appearance, 4 features, 5 updates
    std::function<void()> onFeaturesChanged, onCustomizeTopBar;
    /** The View menu's commands, for the Appearance toggles. */
    void setCommands (juce::ApplicationCommandManager*);
};

} // namespace stagerack
