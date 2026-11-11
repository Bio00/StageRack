#pragma once
// --stress-lifecycle: creates and destroys plugin instances while other instances of the same
// plugin stay alive (optionally processing audio on a separate thread), to reproduce the heap
// corruption seen with some plugins (De-Feedback 1.1.4). Run it under Guard Malloc:
//   DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib $EXE --stress-lifecycle VST3 <plugin> 4 40 --audio
// Usage: --stress-lifecycle <format> <file> <liveInstances> <cycles> [--audio] [--keep]
//   --audio  live instances process noise on a timed thread (32 samples @ 48 kHz) meanwhile
//   --keep   never destroy the cycled instances (only create): what the "safe" mode does live
#include <JuceHeader.h>
#include "Engine/Rack.h"
#include "Plugins/PluginManager.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <sys/resource.h>

namespace stagerack
{

class LifecycleStress final : private juce::Timer
{
public:
    explicit LifecycleStress (const juce::StringArray& args)
    {
        const auto idx = args.indexOf ("--stress-lifecycle");
        formatName = args[idx + 1];
        fileOrId = args[idx + 2];
        numLive = juce::jmax (0, args[idx + 3].getIntValue());
        cycles = juce::jmax (1, args[idx + 4].getIntValue());
        withAudio = args.contains ("--audio");
        keepCycled = args.contains ("--keep");
        if (const auto h = args.indexOf ("--hold"); h >= 0) holdSeconds = args[h + 1].getDoubleValue();
        PluginManager::addAllFormats (fm);
    }

    int start()
    {
        for (auto* f : fm.getFormats())
            if (f->getName() == formatName)
                f->findAllTypesForFile (found, fileOrId);
        if (found.isEmpty()) { std::printf ("plugin not found\n"); return 1; }

        for (int i = 0; i < numLive; ++i)
            if (auto p = create())
                live.push_back (std::move (p));

        std::printf ("live instances: %d, cycles: %d, audio: %s, keep: %s\n", (int) live.size(), cycles,
                     withAudio ? "yes" : "no", keepCycled ? "yes" : "no");
        std::fflush (stdout);

        if (withAudio)
            audio = std::thread ([this] { audioLoop(); });

        startTimer (40);
        return 0;
    }

private:
    static constexpr double sr = 48000.0;
    static constexpr int bs = 32;

    juce::AudioPluginFormatManager fm;
    juce::OwnedArray<juce::PluginDescription> found;
    juce::String formatName, fileOrId;
    int numLive = 0, cycles = 0, step = 0;
    bool withAudio = false, keepCycled = false;
    double holdSeconds = 0;   // --hold s: stay idle with every instance alive, then report CPU used meanwhile

    std::vector<std::unique_ptr<juce::AudioPluginInstance>> live, kept;
    std::unique_ptr<juce::AudioPluginInstance> cycled;
    std::thread audio;
    std::atomic<bool> stop { false };
    std::atomic<long long> blocks { 0 };

    std::unique_ptr<juce::AudioPluginInstance> create()
    {
        juce::String err;
        auto p = fm.createPluginInstance (*found[0], sr, bs, err);
        if (p == nullptr) { std::printf ("create failed: %s\n", err.toRawUTF8()); return {}; }
        Rack::configurePlugin (*p, sr, bs);
        return p;
    }

    static void processSome (juce::AudioPluginInstance& p, int numBlocks, juce::Random& rnd)
    {
        const int ch = juce::jmax (2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buf (ch, bs);
        juce::MidiBuffer midi;
        for (int b = 0; b < numBlocks; ++b)
        {
            for (int c = 0; c < ch; ++c)
                for (int i = 0; i < bs; ++i)
                    buf.setSample (c, i, (rnd.nextFloat() * 2.0f - 1.0f) * 0.1f);
            p.processBlock (buf, midi);
        }
    }

    void audioLoop()
    {
        juce::Random rnd (1);
        const auto period = std::chrono::nanoseconds ((long long) (1.0e9 * bs / sr));
        auto next = std::chrono::steady_clock::now();
        while (! stop.load())
        {
            for (auto& p : live)
                processSome (*p, 1, rnd);
            ++blocks;
            next += period;
            std::this_thread::sleep_until (next);
        }
    }

    void timerCallback() override
    {
        // even steps create (and process a little on this thread), odd steps destroy
        if (step / 2 >= cycles)
            return finish();

        if (step % 2 == 0)
        {
            cycled = create();
            if (cycled != nullptr)
            {
                juce::Random rnd (step);
                processSome (*cycled, 400, rnd);   // > 2 hops of 192 samples, helper thread engaged
            }
        }
        else
        {
            if (keepCycled) kept.push_back (std::move (cycled));
            else            cycled.reset();
            std::printf ("cycle %d ok (audio blocks %lld)\n", step / 2 + 1, blocks.load());
            std::fflush (stdout);
        }
        ++step;
    }

    void finish()
    {
        stopTimer();
        stop = true;
        if (audio.joinable()) audio.join();
        std::printf ("STRESS PASSED: %d cycles, %lld audio blocks\n", cycles, blocks.load());
        if (holdSeconds > 0)
        {
            auto cpu = [] { rusage u {}; getrusage (RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_stime.tv_sec + 1e-6 * (u.ru_utime.tv_usec + u.ru_stime.tv_usec); };
            const auto c0 = cpu();
            std::this_thread::sleep_for (std::chrono::duration<double> (holdSeconds));
            const auto used = cpu() - c0;
            rusage ru {}; getrusage (RUSAGE_SELF, &ru);
            std::printf ("peak memory %.0f MB\n", ru.ru_maxrss / 1048576.0);
            std::printf ("idle with %d instances alive (%d kept) for %.0f s: %.3f s CPU = %.3f%% of one core\n",
                         (int) (live.size() + kept.size()), (int) kept.size(), holdSeconds, used, 100.0 * used / holdSeconds);
        }
        std::fflush (nullptr);
        std::_Exit (0);   // instances deliberately not destroyed here (see CLAUDE.md)
    }
};

} // namespace stagerack
