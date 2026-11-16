#pragma once

#include "Engine/DeviceGuard.h"
#include "Engine/CoreAudioProbe.h"
#include <iostream>

namespace stagerack
{

/** "StageRack --device-guard-test": DeviceGuard with the real CoreAudio devices (outputs stay
    silent: no racks). Drives itself on the message loop, exits with _Exit. */
class DeviceGuardTest final : private juce::Timer
{
public:
    int start()
    {
        if (! coreaudio::waitUntilReady (5000)) { std::cout << "CoreAudio not ready" << std::endl; return 1; }
        next();
        return 0;
    }

private:
    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
        failures += ok ? 0 : 1;
    }

    juce::String builtInOutput()
    {
        juce::AudioDeviceManager dm;
        dm.initialise (0, 2, nullptr, true);
        for (auto* t : dm.getAvailableDeviceTypes())
            for (auto& n : t->getDeviceNames (false))
                if (coreaudio::isBuiltInDevice (n.toRawUTF8()))
                    return n;
        return {};
    }

    void next()
    {
        guard = nullptr;
        engine = nullptr;
        ++step;
        engine = std::make_unique<AudioEngine>();

        if (step == 1)
        {
            // the chosen interface is not connected: nothing opens, status waits for it
            auto xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
            xml->setAttribute ("deviceType", "CoreAudio");
            xml->setAttribute ("audioOutputDeviceName", "StageRack Test Interface");
            xml->setAttribute ("audioInputDeviceName", "StageRack Test Interface");
            guard = std::make_unique<DeviceGuard> (*engine, std::move (xml));
        }
        else if (step == 2)
        {
            // first run, no choice saved: the default (built-in) device is closed and the user asked
            guard = std::make_unique<DeviceGuard> (*engine, nullptr);
            guard->askUseBuiltIn = [this] (std::function<void (bool)>) { ++asked; };
        }
        else if (step == 3)
        {
            // built-in speakers chosen explicitly: used, outputs not blocked
            speakers = builtInOutput();
            auto xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
            xml->setAttribute ("deviceType", "CoreAudio");
            xml->setAttribute ("audioOutputDeviceName", speakers);
            xml->setAttribute ("audioInputDeviceName", "");
            guard = std::make_unique<DeviceGuard> (*engine, std::move (xml));
        }
        else if (step == 4)
        {
            // the chosen device stops (closed without the user): reopened by itself
            auto xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
            xml->setAttribute ("deviceType", "CoreAudio");
            xml->setAttribute ("audioOutputDeviceName", speakers);
            xml->setAttribute ("audioInputDeviceName", "");
            guard = std::make_unique<DeviceGuard> (*engine, std::move (xml));
            guard->start();
            engine->getDeviceManager().closeAudioDevice();
            startTimer (5000);
            return;
        }
        else
        {
            std::cout << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED") << std::endl;
            std::fflush (nullptr);
            std::_Exit (failures == 0 ? 0 : 1);
        }

        guard->start();
        startTimer (1500);   // let the async change messages arrive
    }

    void timerCallback() override
    {
        stopTimer();
        auto* dev = engine->getDeviceManager().getCurrentAudioDevice();

        if (step == 1)
            check (dev == nullptr && guard->getStatus().startsWith ("Waiting for StageRack Test Interface"),
                   "missing interface: no fallback device, status \"" + guard->getStatus() + "\"");
        else if (step == 2)
            check (dev == nullptr && asked == 1 && guard->getStatus().isNotEmpty(),
                   "no choice yet: built-in default closed, user asked once (" + juce::String (asked) + ")");
        else if (step == 3)
            check (speakers.isEmpty() || (dev != nullptr && ! engine->outputsBlocked && guard->getStatus().isEmpty()),
                   "built-in chosen explicitly (" + speakers + "): open, outputs allowed");
        else if (step == 4)
            check (speakers.isEmpty() || (dev != nullptr && guard->getStatus().isEmpty()), "device closed by itself: reopened");

        next();
    }

    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<DeviceGuard> guard;
    juce::String speakers;
    int step = 0, failures = 0, asked = 0;
};

} // namespace stagerack
