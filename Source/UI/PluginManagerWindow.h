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
    SettingsWindow (AudioEngine&, PluginManager&, Session&, std::function<void (bool light)> onThemeChanged);
    void closeButtonPressed() override { setVisible (false); }
};

} // namespace stagerack
