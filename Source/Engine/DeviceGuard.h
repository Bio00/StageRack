#pragma once

#include "AudioEngine.h"

namespace stagerack
{

/**
    Keeps the audio interface the user chose, and never falls back to the Mac's own speakers
    and microphone on its own (on a stage that means feedback).

    JUCE's AudioDeviceManager, when the open device disappears (Dante stopped, USB unplugged),
    opens the system default devices instead and then stays there. Here:
    - the "preferred" device is the last one the user picked (Settings), saved in the settings;
    - if it disappears, whatever JUCE opened in its place is closed (its outputs are silent from
      the first callback: AudioEngine::isDeviceAllowed) and the status says what we wait for;
    - when it shows up again it is reopened, same settings, without asking;
    - built-in devices are used only if the user picked them or confirmed it.
*/
class DeviceGuard final : private juce::ChangeListener,
                          private juce::AudioIODeviceType::Listener,
                          private juce::Timer
{
public:
    /** savedState: the "audioDevice" setting (may be null on the first run). */
    DeviceGuard (AudioEngine&, std::unique_ptr<juce::XmlElement> savedState);
    ~DeviceGuard() override;

    /** Opens the preferred device (or nothing), then watches. Call once CoreAudio answers. */
    void start();

    /** The device the user chose (for the settings file), or null if none yet. */
    const juce::XmlElement* getPreferredState() const noexcept   { return preferred.get(); }

    /** Message for the top bar while no allowed device is open ("Waiting for X..."), else empty. */
    juce::String getStatus() const                                 { return status; }

    /** Called when the preferred state changed (to save it). */
    std::function<void (const juce::XmlElement&)> onPreferredChanged;

    /** Asks the user whether to use the built-in speakers/microphone (no interface chosen yet). */
    std::function<void (std::function<void (bool useBuiltIn)>)> askUseBuiltIn;

    static bool involvesBuiltIn (const juce::AudioDeviceManager::AudioDeviceSetup&);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void audioDeviceListChanged() override;
    void timerCallback() override;
    void evaluate (bool userChange);
    bool preferredIsAvailable() const;
    bool matchesPreferred (const juce::AudioDeviceManager::AudioDeviceSetup&) const;
    bool isAllowed() const;
    void openPreferred();
    void closeDevice();

    juce::String preferredName() const;

    AudioEngine& engine;
    juce::AudioDeviceManager& dm;
    std::unique_ptr<juce::XmlElement> preferred;
    juce::String status;
    bool builtInConfirmed = false, asking = false, started = false;
    int ownChanges = 0;          // change messages caused by us, not by the user
    bool listChanged = false;    // the next change message comes from a device list change
    juce::Array<juce::AudioIODeviceType*> watchedTypes;
    juce::uint32 lastOpenAttempt = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE (DeviceGuard)
};

} // namespace stagerack
