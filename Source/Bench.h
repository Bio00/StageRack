#include <map>
#pragma once

#include "Engine/AudioEngine.h"
#include "Engine/Autosave.h"
#include "Engine/HopAligner.h"
#include "Plugins/PluginManager.h"
#include "Update/Updater.h"
#include "PluginProbe.h"
#include "TempoTest.h"
#include "BudgetTest.h"
#include "RoutingTest.h"
#include "MidiTest.h"
#include <chrono>
#include <thread>
#include <mach/mach.h>
#include <mach/thread_info.h>

namespace stagerack
{
// --threads: every thread of the process with its scheduling priority and CPU time
inline void printThreads (const char* title)
{
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads (mach_task_self(), &list, &count) != KERN_SUCCESS)
        return;

    std::cout << "      threads (" << title << "):" << std::endl;

    for (mach_msg_type_number_t i = 0; i < count; ++i)
    {
        thread_extended_info_data_t info {};
        mach_msg_type_number_t n = THREAD_EXTENDED_INFO_COUNT;

        if (thread_info (list[i], THREAD_EXTENDED_INFO, (thread_info_t) &info, &n) == KERN_SUCCESS
            && (info.pth_user_time + info.pth_system_time) > 20000000)   // > 20 ms of CPU
            std::cout << "        pri " << info.pth_priority << " (cur " << info.pth_curpri << ", policy " << info.pth_policy
                      << ")  cpu " << juce::String ((double) (info.pth_user_time + info.pth_system_time) * 1.0e-9, 2) << " s  "
                      << info.pth_name << std::endl;

        mach_port_deallocate (mach_task_self(), list[i]);
    }

    vm_deallocate (mach_task_self(), (vm_address_t) list, count * sizeof (thread_act_t));
}
}

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
    const auto perRackIdx = args.indexOf ("--per-rack");
    const int perRack = perRackIdx > 0 && args[perRackIdx + 1].getIntValue() > 0 ? args[perRackIdx + 1].getIntValue() : 2;
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
            for (auto* prm : p->getParameters())
                std::cout << "  [" << prm->getParameterIndex() << "] " << prm->getName (64) << " = "
                          << prm->getCurrentValueAsText() << "  (" << prm->getValue() << ")" << std::endl;
        }
        return 0;
    }

    std::cout << "Plugin: " << found[0]->name << " (" << found[0]->pluginFormatName << "), "
              << numRacks << " racks x " << perRack << " plugins, " << bs << " samples @ 48 kHz" << std::endl;

    auto run = [&] (int workers, bool force) -> double
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
        // like the CoreAudio IO thread.
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


        const auto avg = total / blocks;
        const auto budget = 1000.0 * bs / sr;
 std::cout << (force ? "  [forced] " : "  [adaptive] ") << "threads=" << engine.getPool().getNumThreads()
                  << "  avg " << juce::String (avg, 3) << " ms (" << juce::String (100.0 * avg / budget, 1) << "% of buffer)"
                  << "  worst " << juce::String (worst, 3) << " ms  late callbacks " << late << std::endl;

        if (args.contains ("--trace"))
        {
            juce::String csv;
            for (auto v : trace) csv << juce::String (v, 4) << "\n";
            juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("trace_" + juce::String (workers) + (force ? "f" : "") + ".csv").replaceWithText (csv);
        }

        engine.audioDeviceStopped();
        return avg;
    };

    const auto single = run (0, false);
    const auto adaptive = run (-1, false);
    const auto forced = run (-1, true);

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
    Rack::measureCpuTime = true;   // cpuOnHop below
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
            applySetArgs (*p, args);   // --set Name=Value (e.g. EverClean: --set Active=On)
            rack.setPlugin (s, std::move (p));
        }

        // --then <format> <file>: one more plugin after them in each rack (a real chain,
        // e.g. De-Feedback + soothe3 as in a live session)
        if (const auto ti = args.indexOf ("--then"); ti >= 0)
        {
            juce::OwnedArray<juce::PluginDescription> thenDesc;
            for (auto* f : fm.getFormats())
                if (f->getName() == args[ti + 1])
                    f->findAllTypesForFile (thenDesc, args[ti + 2]);

            if (thenDesc.isEmpty()) { std::cout << "--then plugin not found" << std::endl; return 1; }

            juce::String err;
            auto p = fm.createPluginInstance (*thenDesc[0], sr, bs, err);
            if (p == nullptr) { std::cout << "create failed: " << err << std::endl; return 1; }
            Rack::configurePlugin (*p, sr, bs);

            // --then-set Name=Value: like --set, for this plugin ("#1" = normalized value)
            juce::StringArray thenSets;
            for (int k = 0; k < args.size() - 1; ++k)
                if (args[k] == "--then-set")
                    thenSets.addArray ({ "--set", args[k + 1] });
            applySetArgs (*p, thenSets);
            p->prepareToPlay (sr, bs);   // a latency mode takes effect on prepare

            rack.setPlugin (juce::jmin (perRack, Rack::maxSlots - 1), std::move (p));
        }
    }

    // --same-hop-only: phase costs count only instances with the same hop (up to 0.11.4)
    AudioEngine::mixedHopCosts = ! args.contains ("--same-hop-only");

    // --wake-probes N: slow unassisted probes in a row before a type needs CoreWakeAssist
    // (1 = the behaviour up to 0.11.4)
    if (const auto wi = args.indexOf ("--wake-probes"); wi >= 0)
        Rack::wakeNeedProbes = juce::jmax (1, args[wi + 1].getIntValue());

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

    // --buses N: routing on, N empty buses (FX returns); every rack sends to every bus at -10 dB
    if (const auto bi = args.indexOf ("--buses"); bi >= 0)
    {
        const int tracks = engine.getNumRacks();
        engine.setRoutingEnabled (true);

        for (int k = 1; k <= juce::jlimit (1, Rack::maxBuses, args[bi + 1].getIntValue()); ++k)
        {
            auto& bus = engine.addRack();
            bus.outputChannel = 0;
            engine.setRackBus (bus, k);

            for (int r = 0; r < tracks; ++r)
                engine.getRack (r).sends[(size_t) k - 1].db = -10.0f;
        }

        std::cout << "routing: " << (engine.getNumRacks() - tracks) << " buses, each fed by " << tracks << " sends" << std::endl;
    }

    // --master-bus: routing on, one more rack = Bus A with the same plugin; every track outputs to it
    // (a master fed by N tracks: the bus runs one stage after them, in the same callback)
    if (args.contains ("--master-bus"))
    {
        const int tracks = engine.getNumRacks();
        engine.setRoutingEnabled (true);

        auto& bus = engine.addRack();
        bus.outputChannel = 0;
        engine.setRackBus (bus, 1);

        juce::String err;
        auto p = fm.createPluginInstance (*found[0], sr, bs, err);
        if (p == nullptr) { std::cout << "create failed: " << err << std::endl; return 1; }
        Rack::configurePlugin (*p, sr, bs);
        applySetArgs (*p, args);
        bus.setPlugin (0, std::move (p));

        for (int r = 0; r < tracks; ++r)
            engine.setRackOutputBus (engine.getRack (r), 1);

        std::cout << "routing: master bus with the plugin, fed by " << tracks << " tracks" << std::endl;
    }

    struct Proxy final : juce::AudioIODeviceCallback
    {
        std::vector<float> hopCosts;    // rack 0 slot 0 cost on its heavy callbacks
        std::vector<float> eHops, pHops; // same, split by the core type the chain ran on
        std::vector<float> cpuOnHop;     // thread CPU time of the hop (wall - cpu = waiting)

        AudioEngine& engine;
        juce::AudioBuffer<float> noise { 2, 8192 }, scratch { 2, 8192 };
        // one mono "microphone" per rack (device input r), looped; 20 s, a multiple of any block
        juce::AudioBuffer<float> source;
        std::vector<const float*> inPtrs;
        int sourcePos = 0;
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

            // The device is opened without inputs, so the engine would map 0 inputs and the
            // racks would get silence (true for every --bench-device run before this fix):
            // declare our own channels as the device inputs.
            juce::BigInteger ins;
            ins.setRange (0, source.getNumChannels(), true);
            engine.setChannelMaps (ins, source.getNumChannels(), d->getActiveOutputChannels(), d->getOutputChannelNames().size());
            inPtrs.resize ((size_t) source.getNumChannels());
            sourcePos = 0;
        }

        void audioDeviceStopped() override { engine.audioDeviceStopped(); }

        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                               const juce::AudioIODeviceCallbackContext& ctx) override
        {
            const auto t0 = juce::Time::getHighResolutionTicks();
            if (sourcePos + n > source.getNumSamples())
                sourcePos = 0;
            for (int c = 0; c < source.getNumChannels(); ++c)
                inPtrs[(size_t) c] = source.getReadPointer (c, sourcePos);
            sourcePos += n;
            engine.audioDeviceIOCallbackWithContext (inPtrs.data(), (int) inPtrs.size(), scratch.getArrayOfWritePointers(), 2, n, ctx);

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
    AudioEngine::hopFirstOrder = ! args.contains ("--no-hop-order");
    auwg::enabled = ! args.contains ("--no-au-workgroup");

    // --input <file> [startSec]: rack r hears the file's left channel from startSec + 37 s * r
    // (different moments of a real recording = different "microphones"). Default: noise.
    {
        const int len = 48000 * 20;
        proxy.source.setSize (engine.getNumRacks(), len);
        juce::Random rnd (7);

        if (const auto ii = args.indexOf ("--input"); ii >= 0)
        {
            juce::AudioFormatManager afm;
            afm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (afm.createReaderFor (juce::File (args[ii + 1])));
            if (reader == nullptr) { std::cout << "cannot read " << args[ii + 1] << std::endl; return 1; }
            const auto start = args[ii + 2].getDoubleValue();
            juce::AudioBuffer<float> tmp (2, len);
            double rms = 0;

            for (int r = 0; r < engine.getNumRacks(); ++r)
            {
                reader->read (&tmp, 0, len, (juce::int64) ((start + 37.0 * r) * reader->sampleRate), true, true);
                proxy.source.copyFrom (r, 0, tmp, 0, 0, len);
                rms += proxy.source.getRMSLevel (r, 0, len);
            }

            std::cout << "input: " << args[ii + 1] << " from " << start << " s, 20 s per rack, mean RMS "
                      << juce::String (juce::Decibels::gainToDecibels (rms / engine.getNumRacks()), 1) << " dBFS" << std::endl;
        }
        else
        {
            for (int r = 0; r < engine.getNumRacks(); ++r)
                for (int i = 0; i < len; ++i)
                    proxy.source.setSample (r, i, rnd.nextFloat() * 0.2f - 0.1f);
        }

        for (int r = 0; r < engine.getNumRacks(); ++r)
        {
            engine.getRack (r).inputChannel = r;
            engine.getRack (r).stereoInput = false;
        }
    }


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

        // CoreAudio's own overload count (kAudioDeviceProcessorOverload): the real dropouts,
        // including an IO thread woken late, which the callback's own timing cannot see
        juce::Thread::sleep (2000);
        const auto xruns0 = dev->getXRunCount();
        juce::Thread::sleep (10000);
        const auto xruns = dev->getXRunCount() - xruns0;
        std::cout << "      rack 1 input peak (last 12 s): " << juce::String (juce::Decibels::gainToDecibels (engine.getRack (0).inMeter[0].readAndReset()), 1)
                  << " dBFS, output peak " << juce::String (juce::Decibels::gainToDecibels (engine.getRack (0).outMeter[0].readAndReset()), 1) << " dBFS" << std::endl;

        for (int k = 1; k <= Rack::maxBuses; ++k)
            if (auto* bus = engine.findBus (k))
                std::cout << "      bus " << juce::String::charToString ((juce::juce_wchar) ('A' + k - 1)) << " input peak "
                          << juce::String (juce::Decibels::gainToDecibels (bus->inMeter[0].readAndReset()), 1) << " dBFS (stage "
                          << bus->stage << ")" << std::endl;
        // --phases: per slot of rack 1, the hop phase the spread assigned and where the heavy
        // calls really were (last heavyRing ones, modulo the hop)
        if (args.contains ("--phases"))
        {
            auto& r0 = engine.getRack (0);
            for (int i = 0; i < Rack::maxSlots; ++i)
            {
                auto& sl = r0.getSlot (i);
                if (sl.plugin == nullptr) continue;
                const int hop = sl.hopCalls.load();
                juce::String seen;
                const auto n = sl.heavyCount.load();
                std::map<int, int> hist;
                for (juce::uint32 k = 0; k < juce::jmin<juce::uint32> (n, Rack::Slot::heavyRing); ++k)
                    if (hop > 1) ++hist[(int) (sl.heavyAt[k].load() % (juce::uint64) hop)];
                for (auto& [ph, c] : hist) seen << ph << "x" << c << " ";
                std::cout << "      slot " << i << ": hop " << hop << " phase " << sl.hopPhase.load() << "  heavy calls at " << seen << std::endl;
            }
        }

        if (args.contains ("--threads"))
            printThreads ("while running");
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

        std::cout << "      rack 1 hop wait (wall - CPU, smoothed): " << juce::String (engine.getRack (0).getSlot (0).hopWaitMs.load(), 3)
                  << " ms, probe " << juce::String (engine.getRack (0).getSlot (0).wakeProbeMs, 3) << " ms -> wake need " << engine.getRack (0).getSlot (0).wakeNeed.load() << std::endl;
        std::cout << "  " << label << " wake-assist=" << (engine.getWakeAssistMode() == 0 ? "auto" : engine.getWakeAssistMode() == 1 ? "always" : "off") << " workgroup=" << (proxy.workgroup ? "yes" : "no")
                  << "  callbacks " << (n - skip) << "  avg " << juce::String (sum / juce::jmax (1, n - skip), 3)
                  << " ms  worst " << juce::String (worst, 3) << " ms  over budget (" << juce::String (proxy.period, 2)
                  << " ms): " << late << "  device overloads " << xruns << std::endl;

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

    // 5b: input gain (before the plugins) and master mute (all outputs, ramped, racks keep running)
    {
        AudioEngine e;
        auto& r = e.addRack();
        r.inputChannel = 0; r.stereoInput = false; r.outputChannel = 0;
        r.inputGainDb = -6.0f;
        e.prepare (sr, bs, {});

        for (int k = 0; k < 20; ++k) runBlock (e, 1.0f, false);
        check (std::abs (out.getSample (0, bs - 1) - juce::Decibels::decibelsToGain (-6.0f)) < 0.01f,
               "input gain -6 dB (" + juce::String (out.getSample (0, bs - 1), 3) + ")");

        e.masterMuted = true;
        for (int k = 0; k < 20; ++k) runBlock (e, 1.0f, false);
        const auto mutedPeak = r.outMeter[0].readAndReset();
        // getMagnitude (channel, start, num): the two-argument form is (start, num) of all channels
        check (out.getMagnitude (0, 0, bs) < 1.0e-6f && out.getMagnitude (1, 0, bs) < 1.0e-6f && mutedPeak > 0.4f,
               "master mute: outputs silent, rack still metering (meter " + juce::String (mutedPeak, 3) + ")");

        e.masterMuted = false;
        for (int k = 0; k < 20; ++k) runBlock (e, 1.0f, false);
        check (std::abs (out.getSample (0, bs - 1) - juce::Decibels::decibelsToGain (-6.0f)) < 0.01f, "master unmute: back");
        e.audioDeviceStopped();
    }

    // 5c: stereo rack on a one-channel device (Mac microphone): both sides get channel 1
    {
        AudioEngine e;
        auto& r = e.addRack();
        r.inputChannel = 0; r.stereoInput = true; r.outputChannel = 0;
        e.prepare (sr, bs, {});
        for (int k = 0; k < 20; ++k)
        {
            in.clear();
            for (int i = 0; i < bs; ++i) in.setSample (0, i, 0.5f);
            e.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(), 4, bs, ctx);
        }
        check (std::abs (out.getSample (0, bs - 1) - 0.5f) < 0.01f && std::abs (out.getSample (1, bs - 1) - 0.5f) < 0.01f,
               "stereo rack on a mono device: L and R both carry channel 1");
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

    // automixer (gain sharing): one talker = 0 dB, two equal talkers = -3 dB each, zero latency
    {
        float g[3] = { 1.0f, 1.0f, 1.0f };
        const float p1[] = { 1.0f, 0.0f }, p2[] = { 1.0f, 1.0f }, p0[] = { 0.0f, 0.0f }, p3[] = { 1.0f, 1.0f, 2.0f };
        bool ok = AudioEngine::automixGains (p1, g, 2) && std::abs (g[0] - 1.0f) < 1.0e-6f && g[1] <= 1.0e-3f;
        ok = ok && AudioEngine::automixGains (p2, g, 2) && std::abs (g[0] - 0.70711f) < 1.0e-4f && std::abs (g[1] - 0.70711f) < 1.0e-4f;
        ok = ok && ! AudioEngine::automixGains (p0, g, 2) && std::abs (g[0] - 0.70711f) < 1.0e-4f;   // silence: hold
        ok = ok && AudioEngine::automixGains (p3, g, 3) && std::abs (g[0] * g[0] + g[1] * g[1] + g[2] * g[2] - 1.0f) < 1.0e-5f;
        check (ok, "automix law: 1 talker 0 dB, 2 talkers -3 dB, silence holds, sum of powers = 1");

        // overlap modes: two equal talkers -> soft -1.25 dB each, open 0 dB each; in a pause (nobody
        // above its floor) and with a single talker, exactly Dugan
        const float talk2[] = { 1.0f, 1.0f }, talk0[] = { 0.0f, 0.0f }, talk1[] = { 1.0f, 0.0f };
        float h[2];
        AudioEngine::automixGains (p2, h, 2); AudioEngine::automixOverlapGains (h, talk2, 2, 1.5f);
        bool okq = std::abs (juce::Decibels::gainToDecibels (h[0]) + 1.249f) < 0.01f;
        AudioEngine::automixGains (p2, h, 2); AudioEngine::automixOverlapGains (h, talk2, 2, 8.0f);
        okq = okq && std::abs (h[0] - 1.0f) < 1.0e-5f && std::abs (h[1] - 1.0f) < 1.0e-5f;
        AudioEngine::automixGains (p2, h, 2); AudioEngine::automixOverlapGains (h, talk0, 2, 8.0f);
        okq = okq && std::abs (h[0] - 0.70711f) < 1.0e-4f;
        AudioEngine::automixGains (p1, h, 2); AudioEngine::automixOverlapGains (h, talk1, 2, 8.0f);
        okq = okq && std::abs (h[0] - 1.0f) < 1.0e-5f && h[1] <= 1.0e-3f;
        check (okq, "automix overlap: two talkers soft -1.25 / open 0 dB each; pause and one talker = Dugan");

        AudioEngine e;
        auto& a = e.addRack();
        auto& b = e.addRack();
        a.inputChannel = 0; a.stereoInput = false; a.outputChannel = 0; a.automixGroup = 1;
        b.inputChannel = 1; b.stereoInput = false; b.outputChannel = 0; b.automixGroup = 1;
        e.prepare (sr, bs, {});

        juce::AudioBuffer<float> in2 (2, bs);
        // 750 Hz tones (voice band; one period per 64-sample block), same phase: returns the output RMS
        int phase = 0;
        auto feed = [&] (float l, float r, int blocks)
        {
            for (int k = 0; k < blocks; ++k)
            {
                for (int i = 0; i < bs; ++i, ++phase)
                {
                    const auto x = (float) std::sin (2.0 * juce::MathConstants<double>::pi * 750.0 * phase / sr);
                    in2.setSample (0, i, l * x);
                    in2.setSample (1, i, r * x);
                }
                e.audioDeviceIOCallbackWithContext (in2.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 4, bs, ctx);
            }
            return out.getRMSLevel (0, 0, bs);
        };

        auto v = feed (0.5f, 0.0f, 300);
        check (std::abs (v - 0.3536f) < 0.01f && std::abs (a.automixGainDb.load()) < 0.05f && b.automixGainDb.load() <= -59.0f,
               "automix: only A talks -> A at 0 dB (" + juce::String (v, 3) + "), B " + juce::String (b.automixGainDb.load(), 1) + " dB");

        v = feed (0.5f, 0.5f, 300);
        check (std::abs (a.automixGainDb.load() + 3.01f) < 0.1f && std::abs (b.automixGainDb.load() + 3.01f) < 0.1f && std::abs (v - 0.5f) < 0.01f,
               "automix: A and B equal -> -3 dB each (" + juce::String (a.automixGainDb.load(), 2) + " dB, sum " + juce::String (v, 3) + ")");

        b.automixWeightDb = -6.0f;
        feed (0.5f, 0.5f, 300);
        const auto expA = juce::Decibels::gainToDecibels (std::sqrt (1.0f / (1.0f + 0.2512f)));
        check (std::abs (a.automixGainDb.load() - expA) < 0.1f,
               "automix: weight -6 dB on B -> A " + juce::String (a.automixGainDb.load(), 2) + " dB (expected " + juce::String (expA, 2) + ")");
        b.automixWeightDb = 0.0f;

        b.muted = true;
        feed (0.5f, 0.5f, 300);
        check (std::abs (a.automixGainDb.load()) < 0.05f, "automix: muted B leaves the group, A back to 0 dB");
        b.muted = false;

        feed (0.5f, 0.0f, 300);
        v = feed (0.0f, 0.0f, 2000);   // digital silence: nobody below the hold threshold changes
        check (std::abs (a.automixGainDb.load()) < 0.05f, "automix: last mic hold in silence (A " + juce::String (a.automixGainDb.load(), 2) + " dB)");

        e.automixEnabled = false;
        v = feed (0.5f, 0.5f, 4);
        check (std::abs (v - 0.7071f) < 1.0e-3f, "automix off: both racks at unity (" + juce::String (v, 3) + ")");
        e.automixEnabled = true;

        // detector listens to the voice band: 1 kHz on A vs the same RMS at 60 Hz on B
        {
            AudioEngine f;
            auto& fa = f.addRack(); fa.inputChannel = 0; fa.stereoInput = false; fa.outputChannel = 0; fa.automixGroup = 1;
            auto& fb = f.addRack(); fb.inputChannel = 1; fb.stereoInput = false; fb.outputChannel = 0; fb.automixGroup = 1;
            f.prepare (sr, bs, {});
            for (int k = 0, n = 0; k < 400; ++k)
            {
                for (int i = 0; i < bs; ++i, ++n)
                {
                    in2.setSample (0, i, 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * n / sr));
                    in2.setSample (1, i, 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 60.0 * n / sr));
                }
                f.audioDeviceIOCallbackWithContext (in2.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 4, bs, ctx);
            }
            check (fa.automixGainDb.load() > -0.5f && fb.automixGainDb.load() < -15.0f,
                   "automix detector: 60 Hz hum can't take the floor from a 1 kHz talker (" + juce::String (fa.automixGainDb.load(), 1)
                     + " / " + juce::String (fb.automixGainDb.load(), 1) + " dB)");
        }

        // a mic that stops talking hands over within ~0.3 s (hold 40 ms + 100 dB/s)
        {
            feed (0.5f, 0.5f, 300);
            int blocks = 0;
            in2.clear();
            while (blocks < 1000 && a.automixGainDb.load() < -0.5f)
            {
                feed (0.5f, 0.0005f, 1);   // B drops by 60 dB
                ++blocks;
            }
            const auto ms = 1000.0 * blocks * bs / sr;
            check (ms > 30.0 && ms < 400.0, "automix: B stops, A back to -0.5 dB in " + juce::String (ms, 0) + " ms");
        }

        // zero latency: a step on A shows up in the same callback
        feed (0.0f, 0.5f, 300);
        in2.clear();
        in2.setSample (0, 0, 1.0f);
        e.audioDeviceIOCallbackWithContext (in2.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 4, bs, ctx);
        check (std::abs (out.getSample (0, 0)) > 1.0e-4f, "automix: zero latency (impulse in the same callback, at the current share)");

        // cost of the automixer in the callback: 32 racks in one group vs not automixed
        AudioEngine c;
        for (int i = 0; i < 32; ++i) { auto& r = c.addRack(); r.inputChannel = 0; r.stereoInput = false; r.outputChannel = 0; }
        c.prepare (sr, 32, {});
        juce::AudioBuffer<float> in3 (2, 32), out3 (2, 32);
        juce::Random rnd (7);
        for (int i = 0; i < 32; ++i) in3.setSample (0, i, rnd.nextFloat() - 0.5f);
        auto timeIt = [&]
        {
            const auto t0 = juce::Time::getHighResolutionTicks();
            for (int k = 0; k < 20000; ++k)
                c.audioDeviceIOCallbackWithContext (in3.getArrayOfReadPointers(), 2, out3.getArrayOfWritePointers(), 2, 32, ctx);
            return 1.0e6 * juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) / 20000.0;
        };
        const auto off = timeIt();
        for (int i = 0; i < 32; ++i) c.getRack (i).automixGroup = 1;
        const auto on = timeIt();
        std::cout << "  INFO  automix cost, 32 racks @32 smp: " << juce::String (on - off, 2) << " us per callback ("
                  << juce::String (off, 2) << " -> " << juce::String (on, 2) << " us, budget 667 us)" << std::endl;
        check (on - off < 20.0, "automix cost negligible");
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
        e.getRack (0).automixGroup = 2;
        e.getRack (0).automixWeightDb = -3.0f;
        e.automixEnabled = false;
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
        check (e2.getRack (0).automixGroup == 2 && std::abs (e2.getRack (0).automixWeightDb.load() + 3.0f) < 1.0e-4f && ! e2.automixEnabled,
               "session keeps automix group, weight and on/off");

        const auto saved = dir.getChildFile ("Show.stagerack");
        s.saveTo (saved);
        a.tick();
        check (! Autosave::getAutosaveFile().exists(), "autosave: removed after saving the session");

        a.cleanExit();
        check (! Autosave::previousRunCrashed() && ! Autosave::getAutosaveFile().exists(), "autosave: clean quit removes marker and autosave");

        // parked instances: removed/replaced plugins stay alive (not processed) until the session closes
        {
            struct Probe final : juce::AudioProcessorGraph::AudioGraphIOProcessor
            {
                explicit Probe (int& alive) : AudioGraphIOProcessor (audioInputNode), count (alive) { ++count; }
                ~Probe() override { --count; }
                int& count;
            };
            int alive = 0;
            auto& r = e.getRack (0);
            r.setPlugin (0, std::make_unique<Probe> (alive));
            r.setPlugin (1, std::make_unique<Probe> (alive));
            s.removePlugin (r, 0);
            check (alive == 2 && s.getNumRetiredPlugins() == 1 && r.getPlugin (0) == nullptr, "removed plugin is parked, not destroyed");
            s.removeRack (r);
            check (alive == 2 && s.getNumRetiredPlugins() == 2, "deleting a rack parks its plugins");
            s.newSession();
            check (alive == 0 && s.getNumRetiredPlugins() == 0, "closing the session frees the parked plugins");

            // tidy: free what undo cannot bring back, keep at most maxParkedPlugins for undo
            const auto quiet = Session::parkedQuietMs;
            const auto maxParked = Session::maxParkedPlugins;
            auto tidyAll = [&s] { int n = 0; while (s.tidyParkedPlugins()) ++n; return n; };

            auto& t = s.addRack();
            t.setPlugin (0, std::make_unique<Probe> (alive));
            s.markDirty();
            s.removePlugin (t, 0);                                     // an undo step holds it
            s.retirePlugin (std::make_unique<Probe> (alive));          // like a re-spread leftover
            check (tidyAll() == 0 && alive == 2, "tidy: nothing freed before the quiet time");
            Session::parkedQuietMs = 0;
            check (tidyAll() == 1 && alive == 1 && s.getNumRetiredPlugins() == 1, "tidy: leftover freed, the one undo can restore kept");
            check (s.undo() && t.getPlugin (0) != nullptr && alive == 1, "tidy: undo still brings the kept instance back");

            Session::maxParkedPlugins = 2;
            for (int i = 0; i < 4; ++i)
            {
                auto& x = s.addRack();
                x.setPlugin (0, std::make_unique<Probe> (alive));
                s.markDirty();
                s.removeRack (x);
            }
            tidyAll();
            check (s.getNumRetiredPlugins() == 2 && alive == 3, "tidy: above the limit the oldest undo steps go and their instances are freed");
            check (s.undo() && s.getNumRetiredPlugins() == 1 && alive == 3, "tidy: the newest deletion is still undoable (same instance back)");

            Session::parkedQuietMs = quiet;
            Session::maxParkedPlugins = maxParked;
            s.newSession();
            check (alive == 0, "tidy: session close frees the rest");
        }

        Autosave::setDirectoryForTests ({});
        dir.deleteRecursively();
    }

    runTempoSelfTest (check);
    runBudgetSelfTest (check);
    runRoutingSelfTest (check);
    runMidiSelfTest (check);

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
        if (p != nullptr) { Rack::configurePlugin (*p, sr, bs); applySetArgs (*p, args); }
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
                if (! rack->beginHandover (0, twin, r2.hopCalls, r2.phase, call, (int) (warmS * sr), (int) (fadeS * sr)))
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
