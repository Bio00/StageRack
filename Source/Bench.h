#pragma once

#include "Engine/AudioEngine.h"
#include "Engine/Autosave.h"
#include "Engine/HopAligner.h"
#include "Plugins/PluginManager.h"
#include "Update/Updater.h"
#include <chrono>
#include <thread>

namespace stagerack
{

/** "StageRack --bench <format> <fileOrIdentifier> [racks] [blockSize]"
    Loads the same plugin into N racks and times the audio callback single-threaded vs multicore. */
inline int runBenchmark (const juce::StringArray& args)
{
    const auto idx = args.indexOf ("--bench");
    const auto formatName = args[idx + 1];
    const auto id = args[idx + 2];
    const int numRacks = juce::jmax (1, args[idx + 3].getIntValue() > 0 ? args[idx + 3].getIntValue() : 8);
    const int bs = args[idx + 4].getIntValue() > 0 ? args[idx + 4].getIntValue() : 128;
    const int asyncBlock = args[idx + 5].getIntValue();   // optional: run every rack async at this block
    if (args[idx + 6].isNotEmpty()) Rack::asyncSafetyMs = (float) args[idx + 6].getDoubleValue();   // ms
    const int perRack = args[idx + 7].getIntValue() > 0 ? args[idx + 7].getIntValue() : 2;
    const double sr = 48000.0;

    juce::AudioPluginFormatManager fm;
    PluginManager::addAllFormats (fm);

    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats())
        if (f->getName() == formatName)
            f->findAllTypesForFile (found, id);

    if (found.isEmpty())
    {
        std::cout << "plugin not found" << std::endl;
        return 1;
    }

    if (args.contains ("--params"))
    {
        juce::String err;
        if (auto p = fm.createPluginInstance (*found[0], 48000.0, 64, err))
        {
            juce::MemoryBlock mb;
            p->getStateInformation (mb);
            std::cout << "STATE (" << mb.getSize() << " bytes): " << mb.toString().substring (0, 1500) << std::endl;
            juce::File ("/private/tmp/claude-501/-Users-bio--claude-spares-spare-lbu4Gv/72f1c0e3-1d8c-4250-b834-c694f6f26316/scratchpad/defeedback_state.bin").replaceWithData (mb.getData(), mb.getSize());
            for (auto* prm : p->getParameters())
                std::cout << "  [" << prm->getParameterIndex() << "] " << prm->getName (64) << " = "
                          << prm->getCurrentValueAsText() << "  (" << prm->getValue() << ")" << std::endl;
        }
        return 0;
    }

    std::cout << "Plugin: " << found[0]->name << " (" << found[0]->pluginFormatName << "), "
              << numRacks << " racks x " << perRack << " plugins, " << bs << " samples @ 48 kHz" << std::endl;

    auto run = [&] (int workers, bool force, int async) -> double
    {
        AudioEngine engine;
        engine.presetWorkerThreads (workers);
        engine.forceParallel = force;

        for (int r = 0; r < numRacks; ++r)
        {
            auto& rack = engine.addRack();
            juce::String err;

            for (int s = 0; s < perRack; ++s)
            {
                auto p = fm.createPluginInstance (*found[0], sr, bs, err);
                if (p == nullptr) { std::cout << "create failed: " << err << std::endl; return -1; }
                Rack::configurePlugin (*p, sr, bs);

                rack.setPlugin (s, std::move (p));
            }
        }

        engine.prepare (sr, bs, {});

        for (int r = 0; r < engine.getNumRacks(); ++r)
            engine.getRack (r).setAsyncFactor (async / bs);

        juce::AudioBuffer<float> in (2, bs), out (2, bs);
        juce::Random rnd;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < bs; ++i)
                in.setSample (c, i, rnd.nextFloat() * 0.5f - 0.25f);

        juce::AudioIODeviceCallbackContext ctx;
        const int warmup = 2000, blocks = juce::jmax (3000, 48000 * 10 / bs);
        double total = 0, worst = 0;
        int late = 0;
        std::vector<float> trace;
        trace.reserve ((size_t) (warmup + blocks));

        // Paced like a real device (one callback per buffer period) on a realtime thread,
        // like the CoreAudio IO thread, so async threads get real time.
        struct DeviceThread final : juce::Thread
        {
            explicit DeviceThread (std::function<void()> f) : juce::Thread ("bench io"), fn (std::move (f)) {}
            void run() override { fn(); }
            std::function<void()> fn;
        };

        DeviceThread io ([&]
        {
            const auto period = std::chrono::nanoseconds ((juce::int64) (1.0e9 * bs / sr));
            auto next = std::chrono::steady_clock::now();

            for (int b = 0; b < warmup + blocks; ++b)
            {
                const auto t0 = juce::Time::getHighResolutionTicks();
                engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 2, bs, ctx);
                const auto ms = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0;

                if (b == warmup)
                    for (int r = 0; r < engine.getNumRacks(); ++r)
                    {
                    engine.getRack (r).xruns = 0;
                    engine.getRack (r).peakLoad.readAndReset();
                }

                trace.push_back ((float) ms);

                if (b >= warmup)
                {
                    total += ms;
                    worst = juce::jmax (worst, ms);
                    late += ms > 1000.0 * bs / sr ? 1 : 0;
                }

                next += period;
                std::this_thread::sleep_until (next);
            }

        });

        io.startRealtimeThread (juce::Thread::RealtimeOptions{}.withPriority (10).withApproximateAudioProcessingTime (bs, sr));
        while (io.isThreadRunning())
            juce::Thread::sleep (20);

        int xr = 0, latency = 0;
        float peak = 0;
        for (int r = 0; r < engine.getNumRacks(); ++r)
        {
            peak = juce::jmax (peak, engine.getRack (r).peakLoad.readAndReset());
            xr += engine.getRack (r).xruns.load();
            latency = juce::jmax (latency, engine.getRack (r).getAddedLatencySamples());
        }

        const auto avg = total / blocks;
        const auto budget = 1000.0 * bs / sr;
 std::cout << (async > 0 ? "  [async " + juce::String (async) + "] " : (force ? "  [forced] " : "  [adaptive] ")) << "threads=" << engine.getPool().getNumThreads()
                  << "  avg " << juce::String (avg, 3) << " ms (" << juce::String (100.0 * avg / budget, 1) << "% of buffer)"
                  << "  worst " << juce::String (worst, 3) << " ms  late callbacks " << late 
                  << (async > 0 ? "  async xruns " + juce::String (xr) + "  +lat " + juce::String (1000.0 * latency / sr, 2) + " ms  chain peak " + juce::String (peak * 100.0f, 0) + "% of block" : juce::String())
                  << std::endl;

        if (args.contains ("--trace"))
        {
            juce::String csv;
            for (auto v : trace) csv << juce::String (v, 4) << "\n";
            juce::File ("/private/tmp/claude-501/-Users-bio--claude-spares-spare-lbu4Gv/72f1c0e3-1d8c-4250-b834-c694f6f26316/scratchpad/trace_" + juce::String (workers) + (force ? "f" : "") + ".csv").replaceWithText (csv);
        }

        engine.audioDeviceStopped();
        return avg;
    };

    const bool asyncOnly = args.contains ("--async-only");
    const auto single = asyncOnly ? 0.0 : run (0, false, 0);
    const auto adaptive = asyncOnly ? 0.0 : run (-1, false, 0);
    const auto forced = asyncOnly ? 0.0 : run (-1, true, 0);

    if (asyncBlock > 0)
        run (-1, false, asyncBlock);

    if (single > 0 && adaptive > 0 && forced > 0)
        std::cout << "Speed-up avg: adaptive x" << juce::String (single / adaptive, 2)
                  << "  forced x" << juce::String (single / forced, 2) << std::endl;

    return 0;
}

} // namespace stagerack

#include <sys/sysctl.h>

namespace stagerack
{

/** Number of E-cores (they have the lowest CPU numbers on Apple Silicon); 0 on Intel. */
inline int numEfficiencyCpus()
{
    int n = 0;
    size_t size = sizeof (n);
    return sysctlbyname ("hw.perflevel1.logicalcpu", &n, &size, nullptr, 0) == 0 ? n : 0;
}

/** "StageRack --bench-device <format> <id> <racks> <block> [device name part] [--noalign]"
    Same racks as --bench, but driven by a real CoreAudio device (so the IO thread, the audio
    workgroup and its deadline intervals are the real ones). Inputs are replaced by noise and
    outputs are muted. Reports callbacks whose processing took longer than the buffer period. */
inline int runDeviceBenchmark (const juce::StringArray& args)
{
    const auto idx = args.indexOf ("--bench-device");
    const auto formatName = args[idx + 1];
    const auto id = args[idx + 2];
    const int numRacks = juce::jmax (1, args[idx + 3].getIntValue());
    const int bs = juce::jmax (16, args[idx + 4].getIntValue());
    const auto deviceHint = args[idx + 5].isNotEmpty() && ! args[idx + 5].startsWith ("--") ? args[idx + 5] : juce::String ("MacBook");
    const double sr = 48000.0;

    juce::AudioPluginFormatManager fm;
    PluginManager::addAllFormats (fm);

    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats())
        if (f->getName() == formatName)
            f->findAllTypesForFile (found, id);

    if (found.isEmpty()) { std::cout << "plugin not found" << std::endl; return 1; }

    AudioEngine engine;

    for (int r = 0; r < numRacks; ++r)
    {
        auto& rack = engine.addRack();
        rack.inputChannel = 0;
        rack.outputChannel = 0;

        // --per-rack N: N instances in series in each rack
        const auto pi = args.indexOf ("--per-rack");
        const int perRack = pi >= 0 ? juce::jlimit (1, Rack::maxSlots, args[pi + 1].getIntValue()) : 1;

        for (int s = 0; s < perRack; ++s)
        {
            juce::String err;
            auto p = fm.createPluginInstance (*found[0], sr, bs, err);
            if (p == nullptr) { std::cout << "create failed: " << err << std::endl; return 1; }
            Rack::configurePlugin (*p, sr, bs);
            rack.setPlugin (s, std::move (p));
        }
    }

    // --extra N <plugin file>: extra racks with another (light, continuous) plugin
    if (const auto ei = args.indexOf ("--extra"); ei >= 0)
    {
        juce::OwnedArray<juce::PluginDescription> extraDesc;
        for (auto* f : fm.getFormats())
            if (f->getName() == "VST3")
                f->findAllTypesForFile (extraDesc, args[ei + 2]);

        for (int r = 0; r < args[ei + 1].getIntValue() && ! extraDesc.isEmpty(); ++r)
        {
            auto& rack = engine.addRack();
            juce::String err;
            if (auto p = fm.createPluginInstance (*extraDesc[0], sr, bs, err))
            {
                Rack::configurePlugin (*p, sr, bs);
                rack.setPlugin (0, std::move (p));
                rack.bypassed = args.contains ("--extra-bypassed");
            }
        }
    }

    struct Proxy final : juce::AudioIODeviceCallback
    {
        std::vector<float> hopCosts;    // rack 0 slot 0 cost on its heavy callbacks
        std::vector<float> eHops, pHops; // same, split by the core type the chain ran on
        std::vector<float> cpuOnHop;     // thread CPU time of the hop (wall - cpu = waiting)

        AudioEngine& engine;
        juce::AudioBuffer<float> noise { 2, 8192 }, scratch { 2, 8192 };
        std::vector<float> times;
        std::vector<float> perRack = std::vector<float> (60 * 16, 0.0f);
        std::atomic<int> count { 0 };
        double period = 0;
        bool workgroup = false;

        explicit Proxy (AudioEngine& e) : engine (e)
        {
            juce::Random rnd;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < noise.getNumSamples(); ++i)
                    noise.setSample (c, i, rnd.nextFloat() * 0.5f - 0.25f);
            times.resize (200000);
            hopCosts.reserve (200000);
            eHops.reserve (200000);
            pHops.reserve (200000);
            cpuOnHop.reserve (200000);
        }

        void audioDeviceAboutToStart (juce::AudioIODevice* d) override
        {
            period = d->getCurrentBufferSizeSamples() / d->getCurrentSampleRate() * 1000.0;
            workgroup = (bool) d->getWorkgroup();
            engine.audioDeviceAboutToStart (d);
        }

        void audioDeviceStopped() override { engine.audioDeviceStopped(); }

        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                               const juce::AudioIODeviceCallbackContext& ctx) override
        {
            const auto t0 = juce::Time::getHighResolutionTicks();
            const float* in[2] = { noise.getReadPointer (0), noise.getReadPointer (1) };
            engine.audioDeviceIOCallbackWithContext (in, 2, scratch.getArrayOfWritePointers(), 2, n, ctx);

            for (int c = 0; c < numOut; ++c)
                if (out[c] != nullptr)
                    juce::FloatVectorOperations::clear (out[c], n);

            const auto i = count.fetch_add (1);

            if (const auto c0 = engine.getRack (0).getSlot (0).cpuMs.load(); c0 > 0.1f && hopCosts.size() < hopCosts.capacity())
            {
                hopCosts.push_back (c0);
                const auto cpu = engine.getRack (0).lastCpu.load();
                cpuOnHop.push_back (engine.getRack (0).cpuTimeMs.load());
                (cpu >= 0 && cpu < numEfficiencyCpus() ? eHops : pHops).push_back (c0);
            }


            if (i >= 4000 && i < 4000 + 60)
                for (int r = 0; r < engine.getNumRacks() && r < 16; ++r)
                    perRack[(size_t) ((i - 4000) * 16 + r)] = engine.getRack (r).getSlot (0).cpuMs.load();

            if (i < (int) times.size())
                times[(size_t) i] = (float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0);
        }
    } proxy (engine);
    engine.hopAlignEnabled = ! args.contains ("--noalign");


    juce::AudioDeviceManager dm;
    dm.initialise (0, 2, nullptr, false);

    auto setup = dm.getAudioDeviceSetup();
    for (auto* type : dm.getAvailableDeviceTypes())
        for (auto name : (type->scanForDevices(), type->getDeviceNames (false)))
            if (name.containsIgnoreCase (deviceHint))
                setup.outputDeviceName = name;

    setup.inputDeviceName = {};
    setup.bufferSize = bs;
    setup.sampleRate = sr;
    setup.useDefaultInputChannels = false;
    const auto err = dm.setAudioDeviceSetup (setup, true);

    auto* dev = dm.getCurrentAudioDevice();
    if (dev == nullptr) { std::cout << "device error: " << err << std::endl; return 1; }

    std::cout << found[0]->name << " x" << numRacks << " on '" << dev->getName() << "' "
              << dev->getCurrentBufferSizeSamples() << " smp @ " << dev->getCurrentSampleRate() << std::endl;

    auto runFor = [&] (const char* label, int workers)
    {
        engine.presetWorkerThreads (workers);
        engine.setWakeAssistMode (args.contains ("--nowake") ? 2 : (args.contains ("--wake-always") ? 1 : 0));
        if (const auto wi = args.indexOf ("--wake-spinners"); wi >= 0)
            engine.setWakeAssistSpinners (args[wi + 1].getIntValue());
        if (const auto li = args.indexOf ("--wake-lead"); li >= 0)
            engine.wakeLeadMs = args[li + 1].getDoubleValue();
        proxy.count = 0;
        proxy.hopCosts.clear();
        proxy.eHops.clear();
        proxy.pHops.clear();
        proxy.cpuOnHop.clear();
        dm.addAudioCallback (&proxy);

        juce::Thread::sleep (12000);
        dm.removeAudioCallback (&proxy);

        const int n = juce::jmin ((int) proxy.count.load(), (int) proxy.times.size());
        const int skip = juce::jmin (n / 5, 3000);
        int late = 0;
        double worst = 0, sum = 0;
        for (int i = skip; i < n; ++i)
        {
            const auto t = proxy.times[(size_t) i];
            late += t > proxy.period ? 1 : 0;
            worst = juce::jmax (worst, (double) t);
            sum += t;
        }
        if (args.contains ("--trace"))
            for (int c = 0; c < 60; ++c)
            {
                juce::String line ("    cb " + juce::String (c).paddedLeft (' ', 2) + ": ");
                for (int r = 0; r < engine.getNumRacks() && r < 16; ++r)
                    line << juce::String (proxy.perRack[(size_t) (c * 16 + r)], 2) << " ";
                std::cout << line << std::endl;
            }

        std::cout << "  " << label << " wake-assist=" << (engine.getWakeAssistMode() == 0 ? "auto" : engine.getWakeAssistMode() == 1 ? "always" : "off") << " workgroup=" << (proxy.workgroup ? "yes" : "no")
                  << "  callbacks " << (n - skip) << "  avg " << juce::String (sum / juce::jmax (1, n - skip), 3)
                  << " ms  worst " << juce::String (worst, 3) << " ms  over budget (" << juce::String (proxy.period, 2)
                  << " ms): " << late << std::endl;

        auto h = proxy.hopCosts;
        if (h.size() > 20)
        {
            std::sort (h.begin(), h.end());
            auto pct = [&] (double q) { return juce::String (h[(size_t) (q * (double) (h.size() - 1))], 3); };
            if (args.contains ("--timeline"))
            {
                juce::String tl ("      timeline (median per 150 hops):");
                for (size_t k = 0; k + 150 <= proxy.hopCosts.size(); k += 150)
                {
                    std::vector<float> w (proxy.hopCosts.begin() + (long) k, proxy.hopCosts.begin() + (long) k + 150);
                    std::nth_element (w.begin(), w.begin() + 75, w.end());
                    tl << " " << juce::String (w[75], 2);
                }
                std::cout << tl << std::endl;
            }

            auto med = [] (std::vector<float> v) { if (v.empty()) return juce::String ("-"); std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end()); return juce::String (v[v.size() / 2], 3); };
            std::cout << "      audio side of the hop: on E-cores " << (int) proxy.eHops.size() << " (median " << med (proxy.eHops)
                      << " ms), on P-cores " << (int) proxy.pHops.size() << " (median " << med (proxy.pHops) << " ms)" << std::endl;
            std::cout << "      hop: wall median " << med (proxy.hopCosts) << " ms, of which CPU time on the audio thread "
                      << med (proxy.cpuOnHop) << " ms (rest = waiting for the plugin's helper)" << std::endl;
            std::cout << "      hop cost (rack 1): n=" << (int) h.size() << "  p50 " << pct (0.5) << "  p90 " << pct (0.9)
                      << "  p99 " << pct (0.99) << "  max " << pct (1.0) << " ms" << std::endl;
        }
    };

    if (! args.contains ("--multicore-only"))
        runFor ("[1 thread]", 0);
    runFor ("[multicore]", -1);
    dm.closeAudioDevice();
    return 0;
}

} // namespace stagerack

namespace stagerack
{

/** "StageRack --selftest": engine checks for chaining, latency compensation and mono outputs. */
inline int runSelfTest()
{
    const int bs = 64;
    const double sr = 48000.0;
    int failures = 0;

    auto check = [&] (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
        failures += ok ? 0 : 1;
    };

    juce::AudioBuffer<float> in (2, bs), out (4, bs);
    juce::AudioIODeviceCallbackContext ctx;

    auto runBlock = [&] (AudioEngine& e, float inputValue, bool impulse)
    {
        in.clear();
        if (impulse) in.setSample (0, 0, 1.0f);
        else         for (int i = 0; i < bs; ++i) in.setSample (0, i, inputValue);
        e.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 4, bs, ctx);
    };

    // 1+2: B is fed by A, B sits BEFORE A in the list: stages must still run A first.
    {
        AudioEngine e;
        auto& b = e.addRack();
        auto& a = e.addRack();
        a.inputChannel = 0; a.stereoInput = false; a.outputChannel = -1; a.gainDb = -6.0f;
        b.outputChannel = 0; b.gainDb = -6.0f;
        check (e.setRackInputSource (b, a.getId()), "chain B <- A accepted");
        e.prepare (sr, bs, {});

        for (int k = 0; k < 100; ++k)
            runBlock (e, 1.0f, false);

        const auto expected = juce::Decibels::decibelsToGain (-12.0f);
        check (std::abs (out.getSample (0, bs - 1) - expected) < 0.01f,
               "gains add up through the chain (-6 dB + -6 dB = " + juce::String (out.getSample (0, bs - 1), 3) + ", expected " + juce::String (expected, 3) + ")");

        runBlock (e, 0.0f, false);
        runBlock (e, 0.0f, true);
        check (out.getSample (0, 0) > 0.2f, "zero latency: impulse comes out in the same callback (" + juce::String (out.getSample (0, 0), 3) + ")");

        check (! e.setRackInputSource (a, b.getId()), "loop A <- B refused");
        check (e.wouldCreateCycle (a, b.getId()), "cycle detection");
        e.audioDeviceStopped();
    }

    // 4: compensation delay is exact
    {
        AudioEngine e;
        auto& r = e.addRack();
        r.inputChannel = 0; r.stereoInput = false; r.outputChannel = 0;
        e.prepare (sr, bs, {});
        r.compensationSamples = 100;

        for (int k = 0; k < 4; ++k) runBlock (e, 0.0f, false);

        int found = -1;
        runBlock (e, 0.0f, true);

        for (int k = 0; k < 4 && found < 0; ++k)
        {
            for (int i = 0; i < bs; ++i)
                if (out.getSample (0, i) > 0.5f) { found = k * bs + i; break; }

            if (found < 0) runBlock (e, 0.0f, false);
        }

        check (found == 100, "latency compensation delays by exactly 100 samples (got " + juce::String (found) + ")");
        e.audioDeviceStopped();
    }

    // 5: mono output = (L+R)/2 on one channel only
    {
        AudioEngine e;
        auto& r = e.addRack();
        r.inputChannel = 0; r.stereoInput = false; r.outputChannel = 2; r.stereoOutput = false;
        e.prepare (sr, bs, {});

        for (int k = 0; k < 20; ++k) runBlock (e, 0.5f, false);

        check (std::abs (out.getSample (2, bs - 1) - 0.5f) < 0.01f && std::abs (out.getSample (3, bs - 1)) < 1.0e-6f
                 && std::abs (out.getSample (0, bs - 1)) < 1.0e-6f,
               "mono output on channel 3 only (" + juce::String (out.getSample (2, bs - 1), 3) + ")");
        e.audioDeviceStopped();
    }

    // 0: updater version order (semver: release > beta.2 > beta.1 > beta > alpha)
    {
        using update::compareVersions;
        const bool ok = compareVersions ("v0.2.0-beta", "0.1.0-beta") > 0
                     && compareVersions ("0.2.0", "0.2.0-beta") > 0
                     && compareVersions ("0.2.0-beta.2", "0.2.0-beta.1") > 0
                     && compareVersions ("0.2.0-beta.10", "0.2.0-beta.9") > 0
                     && compareVersions ("0.2.0-beta.1", "0.2.0-beta") > 0
                     && compareVersions ("0.2.0-beta", "0.2.0-alpha") > 0
                     && compareVersions ("0.10.0", "0.9.9") > 0
                     && compareVersions ("1.0", "1.0.0") == 0
                     && compareVersions ("v0.1.0-beta", "0.1.0-beta") == 0
                     && compareVersions ("0.1.0-beta", "0.1.0") < 0;
        check (ok, "updater version order (0.10.0 > 0.9.9, 0.2.0 > 0.2.0-beta.2 > beta.1 > beta > alpha)");
    }

    // 5b: device with disabled channels: the callback gets only the enabled ones, packed.
    //     Inputs 1-2 and 5-6 enabled -> buffers [0,1,2,3] = device [0,1,4,5]; same for outputs.
    {
        AudioEngine e;
        auto& r = e.addRack();
        r.inputChannel = 4; r.stereoInput = false; r.outputChannel = 4; r.stereoOutput = false;
        e.prepare (sr, bs, {});
        juce::BigInteger active;
        active.setRange (0, 2, true);
        active.setRange (4, 2, true);
        e.setChannelMaps (active, 8, active, 8);

        juce::AudioBuffer<float> in4 (4, bs), out4 (4, bs);
        in4.clear();
        for (int i = 0; i < bs; ++i) in4.setSample (2, i, 1.0f);   // packed 2 = device input 5
        e.audioDeviceIOCallbackWithContext (in4.getArrayOfReadPointers(), 4, out4.getArrayOfWritePointers(), 4, bs, ctx);
        e.audioDeviceIOCallbackWithContext (in4.getArrayOfReadPointers(), 4, out4.getArrayOfWritePointers(), 4, bs, ctx);

        check (std::abs (out4.getSample (2, bs - 1) - 1.0f) < 0.01f && std::abs (out4.getSample (0, bs - 1)) < 1.0e-6f
                 && std::abs (out4.getSample (3, bs - 1)) < 1.0e-6f,
               "disabled channels: IN 5 / OUT 5 hit packed buffer 2 (" + juce::String (out4.getSample (2, bs - 1), 3) + ")");
        e.audioDeviceStopped();
    }

    // 6: latency groups with a real latent plugin (if installed)
    {
        const juce::File so ("/Library/Audio/Plug-Ins/VST3/Smooth Operator Pro.vst3");
        juce::AudioPluginFormatManager fm;
        PluginManager::addAllFormats (fm);
        juce::OwnedArray<juce::PluginDescription> found;

        for (auto* f : fm.getFormats())
            if (f->getName() == "VST3" && so.exists())
                f->findAllTypesForFile (found, so.getFullPathName());

        if (! found.isEmpty())
        {
            AudioEngine e;
            auto& a = e.addRack();
            auto& b = e.addRack();
            auto& c = e.addRack();
            juce::String err;
            auto p = fm.createPluginInstance (*found[0], sr, bs, err);
            Rack::configurePlugin (*p, sr, bs);
            const int lat = p->getLatencySamples();
            a.setPlugin (0, std::move (p));
            e.prepare (sr, bs, {});

            a.latencyGroup = 1;
            b.latencyGroup = 1;
            e.setRackInputSource (c, a.getId());   // c inherits a's latency, no group
            e.updateLatencyCompensation();

            check (lat > 0 && a.compensationSamples == 0 && b.compensationSamples == lat,
                   "latency group A: plugin rack +0, empty rack +" + juce::String (b.compensationSamples.load())
                     + " (plugin latency " + juce::String (lat) + ")");
            check (c.totalLatencySamples == lat, "chained rack inherits upstream latency (" + juce::String (c.totalLatencySamples.load()) + ")");
            e.audioDeviceStopped();
        }
        else
        {
            std::cout << "  SKIP  latency groups (Smooth Operator Pro not installed)" << std::endl;
        }
    }

    // autosave: written only when the document changed, restorable, removed on a clean quit
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("StageRackAutosaveTest");
        dir.deleteRecursively();
        dir.createDirectory();
        Autosave::setDirectoryForTests (dir);

        AudioEngine e;
        auto* pm = new PluginManager();          // leaked on purpose: its destructor saves files
        Session s (e, *pm);
        s.newSession();
        Autosave a (s);

        check (! Autosave::previousRunCrashed(), "autosave: no marker before start");
        a.start();
        a.tick();
        check (Autosave::previousRunCrashed() && ! Autosave::getAutosaveFile().exists(),
               "autosave: marker written, nothing autosaved for an unchanged session");
        check (! a.saveIfChanged(), "autosave: unchanged session not written");

        e.getRack (0).gainDb = -7.5f;
        e.getRack (0).name = "Lead vox";
        check (a.saveIfChanged() && Autosave::getAutosaveFile().existsAsFile(), "autosave: change written");
        check (! a.saveIfChanged(), "autosave: same state not written twice");

        auto rec = Autosave::readRecovered();
        AudioEngine e2;
        Session s2 (e2, *pm);
        if (rec.root.isValid())
            s2.loadFromTree (rec.root, rec.sourceFile, true);
        check (rec.root.isValid() && e2.getNumRacks() == 1 && std::abs (e2.getRack (0).gainDb.load() + 7.5f) < 1.0e-4f
                 && e2.getRack (0).name == "Lead vox" && s2.isDirty() && s2.getFile() == juce::File(),
               "autosave: recovered session has the change and is marked unsaved");

        const auto saved = dir.getChildFile ("Show.stagerack");
        s.saveTo (saved);
        a.tick();
        check (! Autosave::getAutosaveFile().exists(), "autosave: removed after saving the session");

        a.cleanExit();
        check (! Autosave::previousRunCrashed() && ! Autosave::getAutosaveFile().exists(), "autosave: clean quit removes marker and autosave");

        Autosave::setDirectoryForTests ({});
        dir.deleteRecursively();
    }

    std::cout << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}

/** Test signal of the spread/hand-over tests: synthetic (modulated noise, chord, swelling
    1 kHz tone), or `--input <file> [startSeconds]`. */
inline juce::AudioBuffer<float> makeSpreadTestInput (const juce::StringArray& args, int total, double sr)
{
    // 12 s test signal: modulated noise ("voice"), a chord, and a 1 kHz tone that swells
    // from 3 s on like a feedback ring. Same for every instance.
    juce::AudioBuffer<float> input (2, total);
    juce::Random rnd (1234);
    float pink = 0.0f;
    for (int i = 0; i < total; ++i)
    {
        const double t = i / sr;
        pink = 0.97f * pink + 0.03f * (rnd.nextFloat() * 2.0f - 1.0f);
        const auto env = (float) (0.5 + 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 3.0 * t));
        auto x = 0.6f * pink * env
               + 0.03f * (float) (std::sin (2 * juce::MathConstants<double>::pi * 220.0 * t)
                                  + std::sin (2 * juce::MathConstants<double>::pi * 277.2 * t)
                                  + std::sin (2 * juce::MathConstants<double>::pi * 329.6 * t));
        if (t > 3.0)
            x += (float) (juce::jmin (1.0, (t - 3.0) / 2.0) * 0.1 * std::sin (2 * juce::MathConstants<double>::pi * 1000.0 * t));
        input.setSample (0, i, x);
        input.setSample (1, i, x);
    }

    // --input <audio file> [startSeconds]: real material instead of the synthetic signal
    if (const auto ii = args.indexOf ("--input"); ii >= 0)
    {
        juce::AudioFormatManager afm;
        afm.registerBasicFormats();
        if (auto reader = std::unique_ptr<juce::AudioFormatReader> (afm.createReaderFor (juce::File (args[ii + 1]))))
        {
            const auto start = (juce::int64) (args[ii + 2].getDoubleValue() * reader->sampleRate);
            input.clear();
            reader->read (&input, 0, total, start, true, true);
            std::cout << "input file: " << args[ii + 1] << " @ " << reader->sampleRate << " Hz" << std::endl;
        }
    }

    return input;
}

/** "StageRack --spread-test <format> <id> <block> [wavDir]"
    Audio null test of the hop spreading: identical instances fed the same signal, one as
    the live engine leaves it with spreading OFF (fresh prepare), the others as with spreading
    ON (measurement silence + 0..hop-1 extra silent calls). Prints level, null depth vs the
    reference, residual of a sustained "feedback" tone and the input->output lag. */
inline int runSpreadTest (const juce::StringArray& args)
{
    const auto idx = args.indexOf ("--spread-test");
    const auto formatName = args[idx + 1];
    const auto id = args[idx + 2];
    const int bs = juce::jmax (16, args[idx + 3].getIntValue());
    const juce::File wavDir (args[idx + 4].isNotEmpty() && ! args[idx + 4].startsWith ("--") ? args[idx + 4] : juce::String());
    const double sr = 48000.0;

    juce::AudioPluginFormatManager fm;
    PluginManager::addAllFormats (fm);
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats())
        if (f->getName() == formatName)
            f->findAllTypesForFile (found, id);
    if (found.isEmpty()) { std::cout << "plugin not found" << std::endl; return 1; }

    const int total = (int) (12.0 * sr) / bs * bs;
    auto input = makeSpreadTestInput (args, total, sr);

    struct Run { juce::String label; int warm = 0; int extra = 0; juce::AudioBuffer<float> out; };
    std::vector<Run> runs;
    const int warm = juce::jlimit (96, 768, 12288 / bs);   // what HopAligner's measurement feeds
    runs.push_back ({ "OFF (fresh)", 0, 0, {} });
    runs.push_back ({ "OFF again", 0, 0, {} });
    for (int k = 0; k < 12; ++k)
        runs.push_back ({ "ON shift " + juce::String (k), warm, k, {} });

    std::vector<std::unique_ptr<juce::AudioPluginInstance>> keep;   // destroyed only at exit

    for (auto& r : runs)
    {
        juce::String err;
        auto p = fm.createPluginInstance (*found[0], sr, bs, err);
        if (p == nullptr) { std::cout << "create failed: " << err << std::endl; return 1; }
        Rack::configurePlugin (*p, sr, bs);

        const int ch = juce::jmax (2, p->getTotalNumInputChannels(), p->getTotalNumOutputChannels());
        juce::AudioBuffer<float> buf (ch, bs);
        juce::MidiBuffer midi;

        for (int c = 0; c < r.warm + r.extra; ++c)
        {
            buf.clear();
            p->processBlock (buf, midi);
        }

        r.out.setSize (2, total);
        for (int pos = 0; pos < total; pos += bs)
        {
            buf.clear();
            for (int c = 0; c < 2; ++c)
                buf.copyFrom (c, 0, input, c, pos, bs);
            midi.clear();
            p->processBlock (buf, midi);
            for (int c = 0; c < 2; ++c)
                r.out.copyFrom (c, pos, buf, c, 0, bs);
        }

        keep.push_back (std::move (p));
    }

    auto db = [] (double x) { return 20.0 * std::log10 (juce::jmax (1.0e-12, x)); };
    const int from = (int) (6.0 * sr), n = total - from;   // settled part, tone fully on

    auto rms = [&] (const float* d) { double a = 0; for (int i = 0; i < n; ++i) a += (double) d[from + i] * d[from + i]; return std::sqrt (a / n); };
    auto tone = [&] (const float* d)   // 1 kHz amplitude (Goertzel-style correlation)
    {
        double re = 0, im = 0;
        for (int i = 0; i < n; ++i)
        {
            const double w = 2 * juce::MathConstants<double>::pi * 1000.0 * (from + i) / sr;
            re += d[from + i] * std::cos (w); im += d[from + i] * std::sin (w);
        }
        return 2.0 * std::sqrt (re * re + im * im) / n;
    };
    auto lag = [&] (const float* d)
    {
        const float* x = input.getReadPointer (0);
        int best = 0; double bv = -1;
        for (int l = -256; l <= 2048; ++l)
        {
            double a = 0;
            for (int i = 0; i < 48000; i += 2)
                if (from + i - l >= 0 && from + i - l < total) a += (double) d[from + i] * x[from + i - l];
            if (std::abs (a) > bv) { bv = std::abs (a); best = l; }
        }
        return best;
    };

    const auto* ref = runs[0].out.getReadPointer (0);
    const auto refRms = rms (ref);
    std::cout << "Plugin: " << found[0]->name << ", block " << bs << ", measurement silence " << warm << " calls" << std::endl;
    std::cout << "input: level " << juce::String (db (rms (input.getReadPointer (0))), 1) << " dB, 1 kHz tone "
              << juce::String (db (tone (input.getReadPointer (0))), 1) << " dB" << std::endl;

    for (auto& r : runs)
    {
        const auto* d = r.out.getReadPointer (0);
        double diff = 0;
        for (int i = 0; i < n; ++i) { const double e = d[from + i] - ref[from + i]; diff += e * e; }
        diff = std::sqrt (diff / n);

        std::cout << "  " << r.label.paddedRight (' ', 13) << " level " << juce::String (db (rms (d)), 2).paddedLeft (' ', 7)
                  << " dB   tone " << juce::String (db (tone (d)), 1).paddedLeft (' ', 6)
                  << " dB   null vs OFF " << juce::String (diff == 0 ? -999.0 : db (diff / refRms), 1).paddedLeft (' ', 7)
                  << " dB   lag " << lag (d) << std::endl;

        if (wavDir.getFullPathName().isNotEmpty())
        {
            wavDir.createDirectory();
            const auto f = wavDir.getChildFile (r.label.replaceCharacters (" ()", "___") + ".wav");
            f.deleteFile();
            juce::WavAudioFormat wav;
            if (auto os = f.createOutputStream())
                if (auto w = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (os.release(), sr, 2, 24, {}, 0)))
                    w->writeFromAudioSampleBuffer (r.out, 0, total);
        }
    }
    std::fflush (nullptr);
    std::cout.flush();
    std::_Exit (0);   // plugin teardown outside a running message loop is not reliable
}

/** "StageRack --handover-test <format> <id> <block> [--input file start]"
    Offline check of the live re-spread: a rack runs the plugin as the spread load leaves it;
    at 1 s a re-phased twin starts, warms for Session's warm time, then crossfades in.
    Compares with the same rack without hand-over: must be bit-identical before the
    crossfade, without clicks or level dips during it. */
inline int runHandoverTest (const juce::StringArray& args)
{
    const auto idx = args.indexOf ("--handover-test");
    const auto formatName = args[idx + 1];
    const auto id = args[idx + 2];
    const int bs = juce::jmax (16, args[idx + 3].getIntValue());
    const double sr = 48000.0;
    const double warmS = 2.0, fadeS = 0.08, startS = 1.0;

    juce::AudioPluginFormatManager fm;
    PluginManager::addAllFormats (fm);
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats())
        if (f->getName() == formatName)
            f->findAllTypesForFile (found, id);
    if (found.isEmpty()) { std::cout << "plugin not found" << std::endl; return 1; }

    const int total = (int) (8.0 * sr) / bs * bs;
    auto input = makeSpreadTestInput (args, total, sr);
    std::vector<std::unique_ptr<Rack>> keepRacks;
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> keep;

    auto make = [&]
    {
        juce::String err;
        auto p = fm.createPluginInstance (*found[0], sr, bs, err);
        if (p != nullptr) Rack::configurePlugin (*p, sr, bs);
        return p;
    };

    auto runRack = [&] (bool handover, int targetShift, int& hopOut) -> juce::AudioBuffer<float>
    {
        auto rack = std::make_unique<Rack> ((int) keepRacks.size() + 1);
        rack->prepare (sr, bs);
        auto p = make();
        // as a spread load leaves it: measurement silence, phase 0
        int hop = 0;
        const auto res = HopAligner::detectAndAlign (*p, bs, [] (int) { return 0; }, [] { return (juce::uint64) 0; });
        hop = res.hopCalls;
        hopOut = hop;
        rack->setPlugin (0, std::move (p));

        juce::AudioBuffer<float> out (2, total);
        const juce::uint64 startCall = (juce::uint64) (startS * sr) / (juce::uint64) bs;

        for (juce::uint64 call = 0; (int) call * bs < total; ++call)
        {
            if (handover && call == startCall)
            {
                auto twin = make();
                juce::MemoryBlock mb;
                rack->getPlugin (0)->getStateInformation (mb);
                twin->setStateInformation (mb.getData(), (int) mb.getSize());
                const auto r2 = HopAligner::detectAndAlign (*twin, bs, [targetShift] (int h) { return targetShift % juce::jmax (1, h); },
                                                            [call] { return call; });
                std::cout << "  twin: hop " << r2.hopCalls << " calls, phase " << r2.phase << std::endl;
                if (! rack->beginHandover (0, twin, r2.hopCalls, r2.phase, (int) (warmS * sr), (int) (fadeS * sr)))
                    std::cout << "  beginHandover FAILED" << std::endl;
            }

            const float* ins[2] = { input.getReadPointer (0, (int) call * bs), input.getReadPointer (1, (int) call * bs) };
            rack->process (ins, 2, bs, 0);
            for (int c = 0; c < 2; ++c)
                out.copyFrom (c, (int) call * bs, rack->getOutput(), c, 0, bs);

            if (handover && rack->getSlot (0).handoverDone)
                if (auto old = rack->collectHandover (0))
                    keep.push_back (std::move (old));
        }

        keepRacks.push_back (std::move (rack));   // plugins die at _Exit
        return out;
    };

    int hop = 0;
    const auto ref = runRack (false, 0, hop);
    std::cout << "Plugin: " << found[0]->name << ", block " << bs << ", hop " << hop << " calls" << std::endl;

    auto click = [] (const float* d, int a, int b)   // max 2nd difference = how abrupt
    {
        double m = 0;
        for (int i = juce::jmax (2, a); i < b; ++i)
            m = juce::jmax (m, (double) std::abs (d[i] - 2 * d[i - 1] + d[i - 2]));
        return m;
    };
    auto db = [] (double x) { return 20.0 * std::log10 (juce::jmax (1.0e-12, x)); };

    const int fadeA = ((int) (startS * sr) / bs) * bs + (int) (warmS * sr), fadeB = fadeA + (int) (fadeS * sr);
    int failures = 0;

    for (int shift = 1; shift < juce::jmax (2, hop); ++shift)
    {
        int h = 0;
        const auto test = runRack (true, shift, h);
        const auto* r = ref.getReadPointer (0);
        const auto* t = test.getReadPointer (0);

        double preDiff = 0;
        for (int i = 0; i < fadeA; ++i) preDiff = juce::jmax (preDiff, (double) std::abs (r[i] - t[i]));

        // level in 10 ms windows during the fade: worst ratio test/ref (after the fade the
        // difference is the plugin's own frame-grid dependence, shown by the null figure)
        double worstDip = 0, worstRise = 0;
        for (int w = fadeA - 960; w < fadeB; w += 480)
        {
            double er = 0, et = 0;
            for (int i = w; i < w + 480; ++i) { er += (double) r[i] * r[i]; et += (double) t[i] * t[i]; }
            const auto ratio = db (std::sqrt (et / juce::jmax (1.0e-20, er)));
            if (args.contains ("--verbose"))
                std::cout << "    t=" << juce::String ((w - fadeA) / sr * 1000.0, 0) << " ms ref " << juce::String (db (std::sqrt (er / 480)), 1)
                          << " dBFS ratio " << juce::String (ratio, 2) << std::endl;
            // ignore near-silent windows (pauses): their ratio is noise, not a level change
            if (db (std::sqrt (er / 480)) < -60.0)
                continue;
            worstDip = juce::jmin (worstDip, ratio); worstRise = juce::jmax (worstRise, ratio);
        }

        const auto clickRef = click (r, fadeA - 4800, fadeB + 4800), clickTest = click (t, fadeA - 4800, fadeB + 4800);
        double after = 0, afterRef = 0;
        for (int i = fadeB; i < total; ++i) { const double e = t[i] - r[i]; after += e * e; afterRef += (double) r[i] * r[i]; }

        const bool ok = preDiff == 0.0 && clickTest <= clickRef * 1.25 + 1.0e-4 && worstDip > -1.0 && worstRise < 1.0;
        failures += ok ? 0 : 1;
        std::cout << "  shift " << shift << ": before fade " << (preDiff == 0 ? juce::String ("bit-identical") : "DIFF " + juce::String (preDiff))
                  << ", fade level " << juce::String (worstDip, 2) << "/+" << juce::String (worstRise, 2) << " dB"
                  << ", abruptness " << juce::String (clickTest, 5) << " (ref " << juce::String (clickRef, 5) << ")"
                  << ", after: null " << juce::String (db (std::sqrt (after / juce::jmax (1.0e-20, afterRef))), 1) << " dB"
                  << (ok ? "  OK" : "  FAIL") << std::endl;
    }

    std::cout << (failures == 0 ? "HANDOVER OK" : juce::String (failures) + " FAILED") << std::endl;
    std::fflush (nullptr);
    std::cout.flush();
    std::_Exit (failures == 0 ? 0 : 1);
}

} // namespace stagerack
