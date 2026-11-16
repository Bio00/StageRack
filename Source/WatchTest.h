#pragma once
// --watch-test <format> <plugin> <racks> <block> [device] [--param Active] [--noalign] [--seconds 30]
// Real CoreAudio device, N racks with one instance each, all loaded with <param> OFF (EverClean
// loads with Active off). After 3 s the parameter is switched ON in every instance at the same
// moment (like a scene recall or an ⌥-edit): their hops then start in the same callback. Prints,
// every second, callbacks over budget, the worst one, and how the hops are spread over the
// callback phases, while Session::watchHops() moves them apart live. Exits with _Exit.
#include <JuceHeader.h>
#include "Engine/Session.h"
#include "Plugins/PluginManager.h"
#include "PluginProbe.h"

namespace stagerack
{

class HopWatchTest final : private juce::Timer
{
public:
    explicit HopWatchTest (const juce::StringArray& a) : args (a) {}

    int start()
    {
        const auto idx = args.indexOf ("--watch-test");
        const auto formatName = args[idx + 1];
        const auto id = args[idx + 2];
        const int numRacks = juce::jmax (1, args[idx + 3].getIntValue());
        bs = juce::jmax (16, args[idx + 4].getIntValue());
        const auto deviceHint = args[idx + 5].isNotEmpty() && ! args[idx + 5].startsWith ("--") ? args[idx + 5] : juce::String ("MacBook");
        if (const auto pi = args.indexOf ("--param"); pi >= 0) paramName = args[pi + 1];
        if (const auto si = args.indexOf ("--seconds"); si >= 0) seconds = args[si + 1].getIntValue();
        engine.hopAlignEnabled = ! args.contains ("--noalign");

        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : plugins.getFormatManager().getFormats())
            if (f->getName() == formatName)
                f->findAllTypesForFile (found, id);
        if (found.isEmpty()) { std::cout << "plugin not found" << std::endl; return 1; }

        for (int r = 0; r < numRacks; ++r)
        {
            auto& rack = session.addRack();
            rack.inputChannel = 0;
            rack.outputChannel = 0;
            juce::String err;
            auto p = plugins.getFormatManager().createPluginInstance (*found[0], sr, bs, err);
            if (p == nullptr) { std::cout << "create failed: " << err << std::endl; return 1; }
            Rack::configurePlugin (*p, sr, bs);
            setParam (*p, args.contains ("--start-on") ? 1.0f : 0.0f);
            rack.setPlugin (0, std::move (p));
        }

        dm.initialise (0, 2, nullptr, false);
        auto setup = dm.getAudioDeviceSetup();
        for (auto* type : dm.getAvailableDeviceTypes())
            for (auto name : (type->scanForDevices(), type->getDeviceNames (false)))
                if (name.containsIgnoreCase (deviceHint))
                    setup.outputDeviceName = name;
        setup.inputDeviceName = {};
        setup.bufferSize = bs;
        setup.sampleRate = sr;
        dm.setAudioDeviceSetup (setup, true);
        if (dm.getCurrentAudioDevice() == nullptr) { std::cout << "no device" << std::endl; return 1; }

        std::cout << found[0]->name << " x" << numRacks << " on '" << dm.getCurrentAudioDevice()->getName() << "' " << bs
                  << " smp, " << paramName << " off at start, on for all at t=3 s, hop watch "
                  << (engine.hopAlignEnabled ? "on" : "off") << std::endl;

        dm.addAudioCallback (&proxy);
        startTimer (500);
        return 0;
    }

private:
    const juce::StringArray args;
    static constexpr double sr = 48000.0;
    int bs = 32, seconds = 30, ticks = 0;
    juce::String paramName { "Active" };

    AudioEngine engine;
    PluginManager plugins;
    Session session { engine, plugins };
    juce::AudioDeviceManager dm;

    struct Proxy final : juce::AudioIODeviceCallback
    {
        explicit Proxy (AudioEngine& e) : engine (e) {}
        AudioEngine& engine;
        std::atomic<int> late { 0 }, callbacks { 0 };
        std::atomic<float> worst { 0 };
        double periodMs = 1;

        void audioDeviceAboutToStart (juce::AudioIODevice* d) override
        {
            periodMs = 1000.0 * d->getCurrentBufferSizeSamples() / d->getCurrentSampleRate();
            engine.audioDeviceAboutToStart (d);
        }
        void audioDeviceStopped() override { engine.audioDeviceStopped(); }

        void audioDeviceIOCallbackWithContext (const float* const* in, int ni, float* const* out, int no, int n,
                                               const juce::AudioIODeviceCallbackContext& ctx) override
        {
            const auto t0 = juce::Time::getHighResolutionTicks();
            engine.audioDeviceIOCallbackWithContext (in, ni, out, no, n, ctx);
            for (int c = 0; c < no; ++c)
                if (out[c] != nullptr)
                    juce::FloatVectorOperations::clear (out[c], n);
            const auto ms = (float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0);
            ++callbacks;
            late += ms > periodMs ? 1 : 0;
            if (ms > worst.load()) worst = ms;
        }
    } proxy { engine };

    void setParam (juce::AudioPluginInstance& p, float v)
    {
        for (auto* prm : p.getParameters())
            if (prm->getName (64).equalsIgnoreCase (paramName))
                prm->setValueNotifyingHost (v);
    }

    void timerCallback() override
    {
        ++ticks;
        session.watchHops();

        if (ticks == 6 && ! args.contains ("--start-on"))
            for (int r = 0; r < engine.getNumRacks(); ++r)
                if (auto* p = engine.getRack (r).getPlugin (0))
                    setParam (*p, 1.0f);   // all at once

        if (ticks % 2 != 0)
            return;

        // phases of the hops (as tracked), e.g. "6: 2 2 1 1 1 1"
        juce::String phases;
        int hop = 0;
        std::map<int, int> perPhase;
        for (int r = 0; r < engine.getNumRacks(); ++r)
        {
            auto& s = engine.getRack (r).getSlot (0);
            if (s.hopCalls > 0) { hop = s.hopCalls; ++perPhase[s.hopPhase]; }
        }
        for (int r = 0; r < engine.getNumRacks() && args.contains ("--verbose"); ++r)
        {
            auto& s = engine.getRack (r).getSlot (0);
            juce::String seen;
            const auto c = s.heavyCount.load();
            for (auto k = c > 8 ? c - 8 : 0u; k < c; ++k)
                seen << (int) (s.heavyAt[k % Rack::Slot::heavyRing].load() % (juce::uint64) juce::jmax (1, s.hopCalls.load())) << " ";
            std::cout << "    rack " << r << " tracked phase " << s.hopPhase.load() << "  last heavy calls mod hop: " << seen << "  last twin started " << s.twinLateBy.load() << " callbacks late" << std::endl;
        }

        if (hop > 0)
        {
            phases << "hop " << hop << " calls, instances per phase:";
            for (int k = 0; k < hop; ++k) phases << " " << perPhase[k];
        }
        else
        {
            phases << "no hop tracked";
        }

        std::cout << "t=" << juce::String (ticks / 2).paddedLeft (' ', 2) << " s  over budget " << juce::String (proxy.late.exchange (0)).paddedLeft (' ', 4)
                  << "/" << proxy.callbacks.exchange (0) << "  worst " << juce::String (proxy.worst.exchange (0), 3) << " ms   "
                  << phases << "   " << session.getRespreadStatus() << std::endl;

        if (ticks / 2 >= seconds)
        {
            dm.removeAudioCallback (&proxy);
            std::fflush (nullptr);
            std::_Exit (0);
        }
    }
};

} // namespace stagerack
