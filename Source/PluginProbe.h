#pragma once

#include "Engine/HopAligner.h"
#include "Engine/Rack.h"
#include "Plugins/PluginManager.h"
#include "Plugins/ParamTwins.h"
#include <mach/mach.h>
#include <map>

namespace stagerack
{

inline int countProcessThreads()
{
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t n = 0;
    if (task_threads (mach_task_self(), &list, &n) != KERN_SUCCESS)
        return -1;
    for (mach_msg_type_number_t i = 0; i < n; ++i)
        mach_port_deallocate (mach_task_self(), list[i]);
    vm_deallocate (mach_task_self(), (vm_address_t) list, n * sizeof (thread_act_t));
    return (int) n;
}

/** "--set Name=Value" (repeatable): sets plugin parameters by name, value as text. */
inline void applySetArgs (juce::AudioPluginInstance& p, const juce::StringArray& args)
{
    for (int i = 0; i < args.size() - 1; ++i)
        if (args[i] == "--set")
        {
            const auto name = args[i + 1].upToFirstOccurrenceOf ("=", false, false).trim();
            const auto text = args[i + 1].fromFirstOccurrenceOf ("=", false, false).trim();
            for (auto* prm : p.getParameters())
                if (prm->getName (64).equalsIgnoreCase (name))
                {
                    // "#0.5" = a normalized value (some plugins do not parse their own texts, e.g. soothe3's "on")
                    prm->setValueNotifyingHost (text.startsWithChar ('#') ? text.substring (1).getFloatValue() : prm->getValueForText (text));
                    std::cout << "  set " << name << " = " << prm->getCurrentValueAsText() << std::endl;
                }
        }
}

/** "StageRack --plugin-probe <format> <id> <block> [--set Name=Value]... [--noise]"
    Per-call cost of one instance (offline): median, heavy calls, detected hop, threads created. */
inline int runPluginProbe (const juce::StringArray& args)
{
    const auto idx = args.indexOf ("--plugin-probe");
    const auto formatName = args[idx + 1];
    const auto id = args[idx + 2];
    const int bs = juce::jmax (16, args[idx + 3].getIntValue());
    const double sr = 48000.0;

    juce::AudioPluginFormatManager fm;
    PluginManager::addAllFormats (fm);
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats())
        if (f->getName() == formatName)
            f->findAllTypesForFile (found, id);
    if (found.isEmpty()) { std::cout << "plugin not found" << std::endl; return 1; }

    const auto threads0 = countProcessThreads();
    juce::String err;
    auto p = fm.createPluginInstance (*found[0], sr, bs, err);
    if (p == nullptr) { std::cout << "create failed: " << err << std::endl; return 1; }
    const auto threads1 = countProcessThreads();
    Rack::configurePlugin (*p, sr, bs);
    p->prepareToPlay (sr, bs);
    const auto threads2 = countProcessThreads();
    if (args.contains ("--insert"))
        std::cout << "  insert defaults: " << PluginManager::applyInsertDefaults (*p) << std::endl;
    applySetArgs (*p, args);
    if (args.contains ("--set"))
        p->prepareToPlay (sr, bs);   // a latency / quality mode takes effect on prepare

    std::cout << found[0]->name << "  latency " << p->getLatencySamples() << "  threads +" << (threads1 - threads0)
              << " at create, +" << (threads2 - threads1) << " at prepare" << std::endl;

    const bool noise = args.contains ("--noise");
    juce::AudioBuffer<float> buf (juce::jmax (2, p->getTotalNumInputChannels(), p->getTotalNumOutputChannels()), bs);
    juce::Random rnd (1);
    juce::MidiBuffer midi;
    std::vector<double> t;

    for (int i = 0; i < 1500; ++i)
    {
        buf.clear();
        if (noise)
            for (int c = 0; c < 2; ++c)
                for (int s = 0; s < bs; ++s)
                    buf.setSample (c, s, rnd.nextFloat() * 0.2f - 0.1f);
        const auto t0 = juce::Time::getHighResolutionTicks();
        p->processBlock (buf, midi);
        t.push_back (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0);
    }

    std::vector<double> settled (t.begin() + 200, t.end());
    auto sorted = settled;
    std::sort (sorted.begin(), sorted.end());
    double sum = 0;
    for (auto v : settled) sum += v;
    std::cout << "  per call (ms): median " << juce::String (sorted[sorted.size() / 2], 4)
              << "  avg " << juce::String (sum / (double) settled.size(), 4)
              << "  p99 " << juce::String (sorted[sorted.size() * 99 / 100], 4)
              << "  max " << juce::String (sorted.back(), 4) << std::endl;
    std::cout << "  threads after processing +" << (countProcessThreads() - threads2) << std::endl;

    juce::String line;
    for (int i = 1000; i < 1000 + 64; ++i)
        line << juce::String (t[(size_t) i], 3) << " ";
    std::cout << "  calls 1000..1063: " << line << std::endl;

    const auto res = HopAligner::detectAndAlign (*p, bs, [] (int) { return 0; }, [] { return (juce::uint64) 0; });
    std::cout << "  HopAligner (silence): hop " << res.hopCalls << " calls = " << res.hopCalls * bs << " samples" << std::endl;

    // --toggle-test <param>: does the hop phase survive switching the parameter off and on?
    if (const auto ti = args.indexOf ("--toggle-test"); ti >= 0)
    {
        juce::AudioProcessorParameter* prm = nullptr;
        for (auto* x : p->getParameters())
            if (x->getName (64).equalsIgnoreCase (args[ti + 1])) prm = x;

        auto phaseOf = [&] (int calls) -> int   // phase (mod 6) of the heavy calls, counted from now
        {
            std::map<int, int> h;
            for (int i = 0; i < calls; ++i)
            {
                buf.clear();
                const auto t0 = juce::Time::getHighResolutionTicks();
                p->processBlock (buf, midi);
                if (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0 > 0.08)
                    ++h[i % res.hopCalls];
            }
            int best = -1, n = 0;
            for (auto& [k, v] : h) if (v > n) { best = k; n = v; }
            return best;
        };

        for (int offCalls : { 60, 120, 150, 180, 240, 300, 301, 600, 601, 1500, 3001, 1, 3001 })
        {
            const int before = phaseOf (60);
            prm->setValueNotifyingHost (0.0f);
            for (int i = 0; i < offCalls; ++i) { buf.clear(); p->processBlock (buf, midi); }
            prm->setValueNotifyingHost (1.0f);
            // after "before" was measured over 60 calls (multiple of hop), the expected phase
            // if the internal counter keeps running while off: unchanged, counted from 60+offCalls
            const int after = phaseOf (60);
            const int keepRunning = ((before - offCalls) % res.hopCalls + res.hopCalls) % res.hopCalls;
            std::cout << "  off for " << offCalls << " calls: phase " << before << " -> " << after
                      << "  (counter running would give " << keepRunning << ", frozen " << before << ")" << std::endl;
        }
    }

    std::fflush (nullptr);
    std::_Exit (0);   // VST3 instances must not be destroyed off the message loop
}

} // namespace stagerack

namespace stagerack
{
/** "StageRack --param-probe <format> <id> [--set Name=Value]... [--sweep Name] [--all]"
    (--all: also the parameters a host may not automate, e.g. soothe3's latency mode)
    Lists the parameters (index, name, value, text, steps); --sweep writes 0..1 into one parameter
    like a fader would (processing a block after each write) and prints what the plugin reads back. */
inline int runParamProbe (const juce::StringArray& args)
{
    const auto idx = args.indexOf ("--param-probe");
    juce::AudioPluginFormatManager fm;
    PluginManager::addAllFormats (fm);
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats())
        if (f->getName() == args[idx + 1])
            f->findAllTypesForFile (found, args[idx + 2]);
    if (found.isEmpty()) { std::cout << "plugin not found" << std::endl; return 1; }

    juce::String err;
    auto p = fm.createPluginInstance (*found[0], 48000.0, 32, err);
    if (p == nullptr) { std::cout << "create failed: " << err << std::endl; return 1; }
    Rack::configurePlugin (*p, 48000.0, 32);
    p->prepareToPlay (48000.0, 32);
    juce::AudioBuffer<float> buf (juce::jmax (2, p->getTotalNumInputChannels(), p->getTotalNumOutputChannels()), 32);
    juce::MidiBuffer midi;
    auto run = [&] (int n) { for (int i = 0; i < n; ++i) { buf.clear(); p->processBlock (buf, midi); } };

    auto list = [&]
    {
        for (auto* prm : p->getParameters())
            if (prm->isAutomatable() || args.contains ("--all"))
                std::cout << "  [" << prm->getParameterIndex() << "] " << prm->getName (64) << " = " << prm->getValue()
                          << " '" << prm->getCurrentValueAsText() << "' steps " << prm->getNumSteps()
                          << (prm->isDiscrete() ? " discrete" : "") << (prm->isBoolean() ? " bool" : "")
                          << (paramtwins::active (p->getParameters(), prm) != prm ? "  -> in use: " + paramtwins::active (p->getParameters(), prm)->getName (64) : juce::String())
                          << std::endl;
    };
    run (10);
    std::cout << "before:" << std::endl; list();
    applySetArgs (*p, args);
    run (10);
    juce::Thread::sleep (300);
    std::cout << "after --set:" << std::endl; list();

    if (args.contains ("--sweep"))
    {
        const auto name = args[args.indexOf ("--sweep") + 1];
        for (auto* prm : p->getParameters())
            if (prm->getName (64).equalsIgnoreCase (name))
                for (int k = 0; k <= 40; ++k)
                {
                    const auto v = (float) k / 40.0f;
                    prm->beginChangeGesture(); prm->setValueNotifyingHost (v); prm->endChangeGesture();
                    const auto now = prm->getValue();
                    run (4);
                    juce::Thread::sleep (20);
                    std::cout << "  wrote " << v << " -> read " << now << " then " << prm->getValue()
                              << " '" << prm->getCurrentValueAsText() << "'" << std::endl;
                }
    }
    // Does the plugin save a change made while it is not called (true bypass idle)? And after one call?
    if (args.contains ("--idle-state"))
    {
        auto state = [&] { juce::MemoryBlock m; p->getStateInformation (m); return m; };
        const auto base = state();
        std::cout << "state stable without changes: " << (state() == base ? "yes" : "NO") << std::endl;

        for (auto* prm : p->getParameters())
        {
            if (! prm->isAutomatable() || prm->isBoolean() || prm->isMetaParameter()) continue;
            prm->setValueNotifyingHost (prm->getValue() > 0.5f ? 0.2f : 0.8f);
            juce::Thread::sleep (50);
            const auto idle = state();
            run (1);
            const auto called = state();
            std::cout << "changed " << prm->getName (64) << ": saved while idle " << (idle != base ? "yes" : "NO")
                      << ", one call changes the saved state " << (called != idle ? "YES (idle state was stale)" : "no") << std::endl;
            break;
        }
    }
    std::fflush (nullptr);
    std::_Exit (0);
}
} // namespace stagerack
