#pragma once

#include <JuceHeader.h>

namespace stagerack
{

/**
    Audio device settings with an APPLY button (user request 2026-11-22).

    JUCE's AudioDeviceSelectorComponent reopens the device at every click (input, output, rate,
    buffer, each channel): with Dante or on Intel Macs every reopen takes seconds and the panel
    feels frozen. Here the choices are only staged; the device is reopened once, on Apply.
    Capabilities of a device not open yet (rates, buffer sizes, channel names) are read from a
    device object that is created but never opened (cheap), once per choice.

    MIDI inputs switch at once (no reopen involved).
*/
class AudioDevicePanel final : public juce::Component,
                               private juce::ChangeListener
{
public:
    explicit AudioDevicePanel (juce::AudioDeviceManager&);
    ~AudioDevicePanel() override;

    /** Lays itself out for a width; returns its height. */
    int layoutFor (int width);

    void paint (juce::Graphics&) override;
    void resized() override   { layoutFor (getWidth()); }
    void lookAndFeelChanged() override;

    std::function<void()> onHeightChanged;

private:
    /** A grid of on/off channels (or MIDI inputs): click toggles, drag paints. */
    struct CheckGrid final : juce::Component
    {
        juce::StringArray names;
        juce::BigInteger on;
        std::function<void (int, bool)> onToggle;
        int columns = 1;
        static constexpr int rowH = 22;

        int heightFor (int width);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        int indexAt (juce::Point<int>) const;
        bool paintValue = true;
        int lastIndex = -1;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void loadFromDevice();           // staged = what is open now
    void readCapabilities();         // rates, buffers, channel names of the staged devices
    void refreshControls();
    void updateStatus();
    bool isDirty() const;
    void apply();
    juce::AudioIODeviceType* type() const;

    juce::AudioDeviceManager& dm;
    juce::AudioDeviceManager::AudioDeviceSetup staged;
    juce::Array<double> rates;
    juce::Array<int> buffers;
    juce::StringArray inNames, outNames;
    juce::String error;

    juce::Label inLabel, outLabel, rateLabel, bufferLabel, inChLabel, outChLabel, midiLabel, status, hint;
    juce::ComboBox inBox, outBox, rateBox, bufferBox;
    juce::TextButton applyButton { "Apply" }, revertButton { "Revert" };
    juce::TextButton inAll { "All" }, inNone { "None" }, outAll { "All" }, outNone { "None" };
    CheckGrid inGrid, outGrid, midiGrid;
    juce::Viewport inView, outView;   // long channel lists scroll inside (Dante: 64 + 64)
    juce::Array<juce::MidiDeviceInfo> midiDevices;
    int lastHeight = 0;
};

} // namespace stagerack
