#pragma once

#include <JuceHeader.h>
#include "TempoEngine.h"

namespace stagerack
{

/**
    OSC in (UDP) for the tempos: every message goes to TempoEngine::handleOsc on the network
    thread, stamped when it arrives (no trip through the message thread: a tap keeps its time).
    Bundles are opened. Off by default (opening a port can bring up the macOS firewall prompt);
    port and on/off live in the app settings (SETTINGS file), not in the session.
*/
class OscInput final : private juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
{
public:
    explicit OscInput (TempoEngine& t) : tempo (t)  { receiver.addListener (this); }
    ~OscInput() override                             { receiver.removeListener (this); receiver.disconnect(); }

    /** Opens (or closes, enabled = false) the port; false if the port is busy / invalid. */
    bool setEnabled (bool enabled, int port)
    {
        receiver.disconnect();
        listening = false;
        currentPort = port;
        wanted = enabled;

        if (enabled && port > 0 && port < 65536)
            listening = receiver.connect (port);

        return listening || ! enabled;
    }

    bool isWanted() const noexcept                   { return wanted; }
    bool isListening() const noexcept                { return listening; }
    int getPort() const noexcept                     { return currentPort; }
    juce::uint32 getMessageCount() const noexcept    { return messages.load(); }
    juce::String getLastAddress() const              { const juce::SpinLock::ScopedLockType sl (lastLock); return lastAddress; }

    static constexpr int defaultPort = 8000;

private:
    void oscMessageReceived (const juce::OSCMessage& m) override
    {
        const auto t = TempoEngine::nowSec();
        std::vector<float> values;
        values.reserve ((size_t) m.size());

        for (const auto& a : m)
        {
            if (a.isFloat32())      values.push_back (a.getFloat32());
            else if (a.isInt32())   values.push_back ((float) a.getInt32());
            else if (a.isString())  values.push_back (a.getString().getFloatValue());
        }

        const auto address = m.getAddressPattern().toString();
        tempo.handleOsc (address, values, t);
        ++messages;
        const juce::SpinLock::ScopedLockType sl (lastLock);
        lastAddress = address;
    }

    void oscBundleReceived (const juce::OSCBundle& b) override
    {
        for (const auto& e : b)
        {
            if (e.isMessage())     oscMessageReceived (e.getMessage());
            else if (e.isBundle()) oscBundleReceived (e.getBundle());
        }
    }

    TempoEngine& tempo;
    juce::OSCReceiver receiver { "StageRack OSC" };
    bool wanted = false, listening = false;
    int currentPort = defaultPort;
    std::atomic<juce::uint32> messages { 0 };
    juce::SpinLock lastLock;
    juce::String lastAddress;
};

} // namespace stagerack
