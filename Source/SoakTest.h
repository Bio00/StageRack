#pragma once
// --soak <file.stagerack> [device] [--seconds 120] [--block 32] [--input file.wav] [--quiet-ui] [--rt-guard] [--isolate]
// Long run of a REAL session on a real CoreAudio device, with what happens in a show going on at
// the same time on the message thread: scene recalls, plugin parameters, mutes, graph rebuilds,
// racks added and removed, undo snapshots (unless --quiet-ui). Every rack hears its own input
// (noise, or the file at a different moment per channel). Reports every 10 s and at the end:
// late / skipped / silent callbacks (DropoutMonitor), worst callback, how often the audio thread
// waited for a lock or gave up (rt::lockWaits / lockMisses) and, with --rt-guard, every memory
// allocation made by OUR audio code (rt::installGuard). The session file is never written.
// Exits with _Exit (plugins are not destroyed, plugins.xml not saved).
#include <JuceHeader.h>
#include "Engine/Session.h"
#include "Engine/RtLock.h"
#include "Engine/RtGuard.h"
#include "Plugins/PluginManager.h"
#include "Plugins/Sandbox/SandboxedPlugin.h"

namespace stagerack
{

class SoakTest final : private juce::Timer
{
public:
    explicit SoakTest (const juce::StringArray& a) : args (a) {}

    int start()
    {
        const auto idx = args.indexOf ("--soak");
        file = juce::File (args[idx + 1]);
        deviceHint = args[idx + 2].isNotEmpty() && ! args[idx + 2].startsWith ("--") ? args[idx + 2] : juce::String ("MacBook");
        if (const auto i = args.indexOf ("--seconds"); i >= 0) seconds = args[i + 1].getIntValue();
        if (const auto i = args.indexOf ("--block"); i >= 0) bs = args[i + 1].getIntValue();
        actions = ! args.contains ("--quiet-ui");

        if (args.contains ("--rt-guard"))
        {
            rt::installGuard();

            // positive control: an allocation in host scope must be seen, one in plugin scope not
            const auto before = rt::guardHits();
            static void* (*volatile allocate) (size_t) = std::malloc;   // not elided by the optimizer
            static void (*volatile release) (void*) = std::free;
            {
                const rt::HostScope host;
                { const rt::PluginScope plugin; release (allocate (64)); }
                const auto inPlugin = rt::guardHits() - before;
                release (allocate (64));
                const auto inHost = rt::guardHits() - before - inPlugin;
                std::cout << "rt guard self-check: host allocation seen " << inHost << "x, plugin allocation seen "
                          << inPlugin << "x -> " << (inHost >= 1 && inPlugin == 0 ? "OK" : "NOT WORKING") << std::endl;
            }
            rt::resetGuard();
        }

        PluginManager::isolatePlugins = args.contains ("--isolate");
        if (const auto i = args.indexOf ("--sandbox-spin"); i >= 0) sandbox::SandboxedPlugin::defaultSpinMicros = args[i + 1].getDoubleValue();   // EXPERIMENTAL: every plugin in its own process

        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr) { std::cout << "cannot read " << file.getFullPathName() << std::endl; return 1; }
        session.loadFromTree (juce::ValueTree::fromXml (*xml), file, false);

        // inputs: one long mono signal, channel c reads it 37 s * c later
        const int len = 48000 * 120;
        signal.setSize (1, len);
        if (const auto i = args.indexOf ("--input"); i >= 0)
        {
            juce::AudioFormatManager afm;
            afm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (afm.createReaderFor (juce::File (args[i + 1])));
            if (reader == nullptr) { std::cout << "cannot read " << args[i + 1] << std::endl; return 1; }
            juce::AudioBuffer<float> tmp (2, len);
            reader->read (&tmp, 0, len, 0, true, true);
            signal.copyFrom (0, 0, tmp, 0, 0, len);
        }
        else
        {
            juce::Random rnd (3);
            for (int i = 0; i < len; ++i)
                signal.setSample (0, i, (rnd.nextFloat() * 0.2f - 0.1f) * (0.3f + 0.7f * (float) ((i / 24000) % 2)));
        }

        std::cout << "soak: " << file.getFileName() << ", " << seconds << " s at " << bs << " smp, show actions "
                  << (actions ? "ON" : "off") << ", rt guard " << (rt::guardInstalled() ? "ON" : "off")
                  << ", plugins " << (PluginManager::isolatePlugins.load() ? "ISOLATED" : "in process") << std::endl;
        startTimer (100);
        return 0;
    }

private:
    const juce::StringArray args;
    juce::File file;
    juce::String deviceHint;
    int seconds = 120, bs = 32, ticks = 0, step = 0;
    bool actions = true, running = false;
    double startMs = 0, lastReportMs = 0;
    juce::Random rnd { 11 };
    juce::AudioBuffer<float> signal;
    std::vector<Rack*> added;
    juce::StringArray done;

    AudioEngine engine;
    PluginManager plugins;
    Session session { engine, plugins };
    juce::AudioDeviceManager dm;

    struct Proxy final : juce::AudioIODeviceCallback
    {
        explicit Proxy (SoakTest& t) : test (t) {}
        SoakTest& test;
        std::vector<const float*> ins;
        int pos = 0, numIns = 0;
        double periodMs = 1;
        std::atomic<juce::uint32> callbacks { 0 }, late { 0 };
        std::atomic<float> worst { 0.0f }, windowWorst { 0.0f };

        void audioDeviceAboutToStart (juce::AudioIODevice* d) override
        {
            periodMs = 1000.0 * d->getCurrentBufferSizeSamples() / d->getCurrentSampleRate();
            test.engine.audioDeviceAboutToStart (d);
            numIns = 64;
            juce::BigInteger on;
            on.setRange (0, numIns, true);
            test.engine.setChannelMaps (on, numIns, d->getActiveOutputChannels(), d->getOutputChannelNames().size());
            ins.assign ((size_t) numIns, nullptr);
        }

        void audioDeviceStopped() override { test.engine.audioDeviceStopped(); }

        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int no, int n,
                                               const juce::AudioIODeviceCallbackContext& ctx) override
        {
            const auto t0 = juce::Time::getHighResolutionTicks();
            const int len = test.signal.getNumSamples();
            for (int c = 0; c < numIns; ++c)
                ins[(size_t) c] = test.signal.getReadPointer (0, (pos + c * 48000 * 37) % (len - 4096));
            pos = (pos + n) % (len - 4096);

            test.engine.audioDeviceIOCallbackWithContext (ins.data(), numIns, out, no, n, ctx);
            for (int c = 0; c < no; ++c)
                if (out[c] != nullptr)
                    juce::FloatVectorOperations::clear (out[c], n);   // never to the speakers

            const auto ms = (float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0);
            ++callbacks;
            if (ms > periodMs) ++late;
            if (ms > worst.load()) worst = ms;
            if (ms > windowWorst.load()) windowWorst = ms;
        }
    } proxy { *this };

    bool openDevice()
    {
        dm.initialise (0, 2, nullptr, false);
        auto setup = dm.getAudioDeviceSetup();
        for (auto* type : dm.getAvailableDeviceTypes())
            for (auto name : (type->scanForDevices(), type->getDeviceNames (false)))
                if (name.containsIgnoreCase (deviceHint))
                    setup.outputDeviceName = name;
        setup.inputDeviceName = {};
        setup.bufferSize = bs;
        setup.sampleRate = 48000.0;
        dm.setAudioDeviceSetup (setup, true);
        if (dm.getCurrentAudioDevice() == nullptr) return false;
        std::cout << "device '" << dm.getCurrentAudioDevice()->getName() << "' " << dm.getCurrentAudioDevice()->getCurrentBufferSizeSamples()
                  << " smp, " << engine.getNumRacks() << " racks, " << session.getNumScenes() << " scenes" << std::endl;
        dm.addAudioCallback (&proxy);
        return true;
    }

    Rack* randomRack()
    {
        if (engine.getNumRacks() == 0) return nullptr;
        return &engine.getRack (rnd.nextInt (engine.getNumRacks()));
    }

    void showAction()
    {
        // what a sound engineer does during a show, one thing every 100 ms
        switch (step++ % 8)
        {
            case 0:
                if (session.getNumScenes() > 1 && step % 40 == 1) { session.stepScene (1); done.addIfNotAlreadyThere ("scene recall"); }
                break;
            case 1:
            {
                std::vector<juce::AudioProcessorParameter*> params;   // continuous parameters of every plugin
                for (int r = 0; r < engine.getNumRacks(); ++r)
                    for (int sl = 0; sl < Rack::maxSlots; ++sl)
                        if (auto* p = engine.getRack (r).getPlugin (sl))
                            for (auto* prm : p->getParameters())
                                if (prm->isAutomatable() && ! prm->isBoolean() && ! prm->isDiscrete() && ! prm->isMetaParameter())
                                    params.push_back (prm);
                if (! params.empty())
                {
                    auto* prm = params[(size_t) rnd.nextInt ((int) params.size())];
                    prm->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, prm->getValue() + (rnd.nextFloat() - 0.5f) * 0.02f));
                    done.addIfNotAlreadyThere ("plugin parameter");
                }
                break;
            }
            case 2:
                if (auto* r = randomRack()) { r->gainDb = juce::jlimit (-20.0f, 6.0f, r->gainDb.load() + (rnd.nextFloat() - 0.5f)); done.addIfNotAlreadyThere ("fader"); }
                break;
            case 3:
                engine.rebuildRouting();
                done.addIfNotAlreadyThere ("graph rebuild");
                break;
            case 4:
                if (step % 48 == 5)
                {
                    if (added.size() < 2) { added.push_back (&session.addRack()); done.addIfNotAlreadyThere ("add rack"); }
                    else { session.removeRack (*added.front()); added.erase (added.begin()); done.addIfNotAlreadyThere ("remove rack"); }
                }
                break;
            case 5:
                session.markDirty();   // undo snapshot, as every edit makes
                done.addIfNotAlreadyThere ("undo snapshot");
                break;
            case 6:
                if (step % 24 == 7)
                    if (auto* r = randomRack()) { r->muted = ! r->muted.load(); done.addIfNotAlreadyThere ("mute"); }
                break;
            case 7:
                engine.updateLatencyCompensation();
                session.watchHops();
                break;
        }
    }

    void report (bool final)
    {
        auto& d = engine.dropouts;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        std::cout << "  t=" << juce::String (juce::roundToInt ((now - startMs) / 1000.0)).paddedLeft (' ', 4) << " s  callbacks " << proxy.callbacks.load()
                  << "  late " << d.getLate() << "  skipped " << d.getSkipped() << "  silent " << d.getSilent()
                  << "  worst(10 s) " << juce::String (proxy.windowWorst.load(), 3) << " ms"
                  << "  lock waits " << rt::lockWaits.load() << " misses " << rt::lockMisses.load();
        if (rt::guardInstalled()) std::cout << "  host allocations " << rt::guardHits();
        std::cout << std::endl;
        proxy.windowWorst = 0.0f;

        if (final)
        {
            std::cout << "SOAK " << (d.getTotal() == 0 && rt::lockMisses.load() == 0 ? "CLEAN" : "WITH DROPOUTS")
                      << ": " << proxy.callbacks.load() << " callbacks, worst " << juce::String (proxy.worst.load(), 3)
                      << " ms (budget " << juce::String (proxy.periodMs, 3) << "), late " << d.getLate() << ", skipped "
                      << d.getSkipped() << ", silent " << d.getSilent() << std::endl;
            std::cout << "  actions done: " << done.joinIntoString (", ") << std::endl;

            juce::uint32 dry = 0, late = 0; int isolated = 0;
            for (int r = 0; r < engine.getNumRacks(); ++r)
                for (int sl = 0; sl < Rack::maxSlots; ++sl)
                    if (auto* p = dynamic_cast<sandbox::SandboxedPlugin*> (engine.getRack (r).getPlugin (sl)))
                    {
                        ++isolated;
                        dry += p->getDryBlocks();
                        late += p->getLateBlocks();
                    }
            if (isolated > 0)
                std::cout << "  isolated plugins: " << isolated << ", blocks passed dry " << (int) dry << " (late " << (int) late << ")" << std::endl;
            if (rt::guardInstalled())
                std::cout << "  allocations in host audio code: " << rt::guardHits() << "\n" << rt::guardReport() << std::endl;
        }
    }

    void timerCallback() override
    {
        ++ticks;
        if (! running)
        {
            if (session.getPendingLoads() > 0 && ticks < 600)
                return;
            if (! openDevice()) { std::cout << "no device" << std::endl; std::_Exit (1); }
            running = true;
            startMs = lastReportMs = juce::Time::getMillisecondCounterHiRes();
            return;
        }

        const auto now = juce::Time::getMillisecondCounterHiRes();

        // first 5 s: loading / aligning settle; counters restart after it (like the app)
        if (now - startMs < 5000.0)
            return;
        if (! settled)
        {
            settled = true;
            engine.dropouts.clearCounters();
            rt::lockWaits = 0; rt::lockMisses = 0;
            rt::resetGuard();
            proxy.worst = 0.0f; proxy.late = 0;
            startMs = lastReportMs = now;
            std::cout << "  settled, counting from now" << std::endl;
            return;
        }

        if (actions)
            showAction();

        if (now - lastReportMs >= 10000.0)
        {
            lastReportMs = now;
            report (false);
        }

        if (now - startMs >= seconds * 1000.0)
        {
            report (true);
            std::cout.flush();
            std::_Exit (0);
        }
    }

    bool settled = false;
};

} // namespace stagerack
