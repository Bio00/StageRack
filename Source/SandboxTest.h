#pragma once
// --sandbox-test <format> <id> [block] : EXPERIMENTAL plugin isolation, checked against the same
// plugin in process. 1) null test (same state, same input: output must be identical), 2) round
// trip cost per block, 3) a parameter set on the proxy reaches the child, 4) the child crashes:
// the slot passes dry, the child comes back by itself with the state, 5) the child hangs: blocks
// pass dry meanwhile, then it resumes. Message loop driven, exits with _Exit.
#include <JuceHeader.h>
#include "Plugins/PluginManager.h"
#include "Plugins/Sandbox/SandboxedPlugin.h"
#include "Engine/Rack.h"

namespace stagerack
{

class SandboxTest final : private juce::Timer
{
public:
    explicit SandboxTest (const juce::StringArray& a) : args (a) {}

    int start()
    {
        const auto idx = args.indexOf ("--sandbox-test");
        if (args[idx + 3].getIntValue() > 0) bs = args[idx + 3].getIntValue();

        juce::AudioPluginFormatManager fm;
        PluginManager::addAllFormats (fm);
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : fm.getFormats())
            if (f->getName() == args[idx + 1])
                f->findAllTypesForFile (found, args[idx + 2]);
        if (found.isEmpty()) { std::cout << "plugin not found" << std::endl; return 1; }
        desc = *found[0];

        juce::String err;
        local = fm.createPluginInstance (desc, sr, bs, err);
        if (local == nullptr) { std::cout << "in-process create failed: " << err << std::endl; return 1; }
        Rack::configurePlugin (*local, sr, bs);

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        iso = sandbox::SandboxedPlugin::createBlocking (desc, sr, bs, err);
        if (iso == nullptr) { std::cout << "sandbox create failed: " << err << std::endl; return 1; }
        Rack::configurePlugin (*iso, sr, bs);
        std::cout << desc.name << " (" << desc.pluginFormatName << "), block " << bs << ": sandbox started in "
                  << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - t0) << " ms, "
                  << iso->getParameters().size() << " parameters, " << iso->getTotalNumInputChannels() << " in / "
                  << iso->getTotalNumOutputChannels() << " out" << std::endl;

        juce::MemoryBlock state;
        local->getStateInformation (state);
        iso->setStateInformation (state.getData(), (int) state.getSize());

        nullTest ("null test (same state, same input)");
        startTimer (100);
        return 0;
    }

private:
    const juce::StringArray args;
    static constexpr double sr = 48000.0;
    int bs = 32, ticks = 0, step = 0, failures = 0, crashTick = 0, paramIndex = -1;
    juce::PluginDescription desc;
    std::unique_ptr<juce::AudioPluginInstance> local;
    std::unique_ptr<sandbox::SandboxedPlugin> iso;
    juce::Random rnd { 5 };
    juce::String textBefore;
    juce::uint32 dryAtRestart = 0;

    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
        if (! ok) ++failures;
    }

    juce::AudioBuffer<float> noise (int chans)
    {
        juce::AudioBuffer<float> b (chans, bs);
        for (int c = 0; c < chans; ++c)
            for (int i = 0; i < bs; ++i)
                b.setSample (c, i, rnd.nextFloat() * 0.5f - 0.25f);
        return b;
    }

    void nullTest (const juce::String& label)
    {
        // correctness only: a slow (cold) block must not turn dry here (that is the hang test's job)
        iso->setWaitFractionForTests (1000.0);
        struct Restore { sandbox::SandboxedPlugin& p; ~Restore() { p.setWaitFractionForTests (0.8); } } restore { *iso };
        const int chans = juce::jmax (local->getTotalNumInputChannels(), local->getTotalNumOutputChannels());
        juce::MidiBuffer midi;
        double worstDiff = 0, sumRt = 0, maxRt = 0;
        const int blocks = 6000;

        for (int k = 0; k < blocks; ++k)
        {
            auto a = noise (chans);
            juce::AudioBuffer<float> b; b.makeCopyOf (a);
            local->processBlock (a, midi);
            iso->processBlock (b, midi);
            for (int c = 0; c < local->getTotalNumOutputChannels(); ++c)
                for (int i = 0; i < bs; ++i)
                    worstDiff = juce::jmax (worstDiff, (double) std::abs (a.getSample (c, i) - b.getSample (c, i)));
            sumRt += iso->getRoundTripMs();
            maxRt = juce::jmax (maxRt, iso->getRoundTripMs());
        }

        std::cout << "  " << label << ": max difference " << worstDiff << ", round trip avg "
                  << juce::String (1000.0 * sumRt / blocks, 1) << " us, max " << juce::String (1000.0 * maxRt, 1)
                  << " us, dry blocks " << (int) iso->getDryBlocks() << std::endl;
        check (worstDiff == 0.0 && iso->getDryBlocks() == dryAtRestart, label + ": bit-identical to the plugin in process");
    }

    float processOne (bool& wasDry)
    {
        // returns how far the output is from the input (0 = dry)
        const int chans = juce::jmax (iso->getTotalNumInputChannels(), iso->getTotalNumOutputChannels());
        auto a = noise (chans);
        juce::AudioBuffer<float> in; in.makeCopyOf (a);
        juce::MidiBuffer midi;
        const auto dryBefore = iso->getDryBlocks();
        { juce::AudioBuffer<float> twin; twin.makeCopyOf (in); local->processBlock (twin, midi); }   // same history
        iso->processBlock (a, midi);
        wasDry = iso->getDryBlocks() != dryBefore;
        float d = 0;
        for (int i = 0; i < bs; ++i) d = juce::jmax (d, std::abs (a.getSample (0, i) - in.getSample (0, i)));
        return d;
    }

    void timerCallback() override
    {
        ++ticks;

        if (step == 0)
        {
            // a continuous parameter set on the proxy shows up in the child (its text changes)
            for (auto* p : iso->getParameters())
                if (! p->isDiscrete() && ! p->isBoolean() && p->isAutomatable())
                {
                    paramIndex = p->getParameterIndex();
                    textBefore = p->getCurrentValueAsText();
                    const auto v = p->getValue() < 0.5f ? 0.9f : 0.1f;
                    p->setValueNotifyingHost (v);
                    local->getParameters()[paramIndex]->setValueNotifyingHost (v);   // same history for both
                    break;
                }
            step = 1;
            return;
        }

        if (step == 1)
        {
            if (ticks < 6) return;   // child publishes values / texts at 30 Hz
            if (paramIndex >= 0)
            {
                bool dry;
                processOne (dry);   // the audio thread applies the change (or the child's idle timer does)
                juce::Thread::sleep (100);
                const auto now = iso->parameterText (paramIndex);
                std::cout << "  parameter '" << iso->getParameters()[paramIndex]->getName (40) << "': '" << textBefore << "' -> '" << now << "'" << std::endl;
                check (now.isNotEmpty() && now != textBefore, "a parameter set on the rack reaches the plugin in its process");
            }

            {
                // same state + reset, no crash: identical again?
                juce::MemoryBlock state;
                local->getStateInformation (state);
                iso->setStateInformation (state.getData(), (int) state.getSize());
                local->reset(); iso->reset();
                juce::Thread::sleep (100);
                nullTest ("state + reset, no crash");
            }

            juce::MemoryBlock keep;
            iso->getStateInformation (keep);   // the cache a restart uses
            std::cout << "  crashing the sandbox process..." << std::endl;
            iso->crashChildForTests();
            crashTick = ticks;
            step = 2;
            return;
        }

        if (step == 2)
        {
            if (! (iso->getRestartCount() > 0 && iso->isChildAlive()))
            {
                bool dry = false;
                processOne (dry);   // while it is gone: dry
            }

            if (iso->getRestartCount() > 0 && iso->isChildAlive())
            {
                std::cout << "  back after " << (ticks - crashTick) * 100 << " ms, " << (int) iso->getDryBlocks() << " dry blocks meanwhile" << std::endl;
                check (iso->getDryBlocks() > 0, "while the process was gone the slot passed its input (dry), no silence, no crash");
                juce::Thread::sleep (200);
                // the child restarted with a FRESH instance + the cached state: compare with a fresh
                // instance in process with the same state
                juce::MemoryBlock state;
                iso->getStateInformation (state);
                juce::AudioPluginFormatManager fm;
                PluginManager::addAllFormats (fm);
                juce::String err;
                local = fm.createPluginInstance (desc, sr, bs, err);
                Rack::configurePlugin (*local, sr, bs);
                local->setStateInformation (state.getData(), (int) state.getSize());
                juce::Thread::sleep (100);
                for (int i = 0; i < iso->getParameters().size(); ++i)
                    if (iso->parameterText (i) != local->getParameters()[i]->getCurrentValueAsText())
                        std::cout << "    differs: " << local->getParameters()[i]->getName (40) << " local '" << local->getParameters()[i]->getCurrentValueAsText()
                                  << "' isolated '" << iso->parameterText (i) << "'" << std::endl;
                dryAtRestart = iso->getDryBlocks();
                nullTest ("after the restart");
                step = 3;
            }
            else if (ticks - crashTick > 300)
            {
                check (false, "the sandbox process came back within 30 s");
                finish();
            }
            return;
        }

        if (step == 3)
        {
            // hung plugin: 300 ms stuck in its audio code
            iso->hangChildForTests (300);
            juce::Thread::sleep (30);
            int dryCount = 0, blocks = 0;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            while (juce::Time::getMillisecondCounterHiRes() - t0 < 600.0)
            {
                bool dry;
                processOne (dry);
                dryCount += dry ? 1 : 0;
                ++blocks;
                juce::Thread::sleep (1);
            }
            bool dryAfter = true;
            for (int k = 0; k < 20; ++k) processOne (dryAfter);
            std::cout << "  hang 300 ms: " << dryCount << " of " << blocks << " blocks dry, late " << (int) iso->getLateBlocks() << std::endl;
            check (dryCount > 0 && ! dryAfter, "a hung plugin: its blocks pass dry meanwhile, then it resumes");
            finish();
        }
    }

    void finish()
    {
        std::cout << (failures == 0 ? "SANDBOX TEST PASSED" : "SANDBOX TEST FAILED") << std::endl;
        std::cout.flush();
        std::_Exit (failures == 0 ? 0 : 1);
    }
};

} // namespace stagerack
