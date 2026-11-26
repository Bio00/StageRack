#include "RtLock.h"
#include "RtGuard.h"
#include "Rack.h"
#include "Features.h"
#include "TempoEngine.h"
#include "WorkerPool.h"
#include <pthread.h>
#include <ctime>

namespace stagerack
{

//==============================================================================
Rack::Rack (int uniqueId) : id (uniqueId)
{
    delayLine.setSize (2, maxCompensation);
    delayLine.clear();
    buffer.setSize (maxPluginChannels, deviceBlockSize);
    preBuffer.setSize (2, deviceBlockSize);
    preBuffer.clear();
    gain.reset (sampleRate, 0.02);
}

Rack::~Rack()
{
    release();
}

//==============================================================================
void Rack::configurePlugin (juce::AudioPluginInstance& p, double sr, int bs)
{
    p.disableNonMainBuses();

    auto tryLayout = [&p] (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
    {
        auto layout = p.getBusesLayout();

        if (layout.inputBuses.size() > 0)  layout.inputBuses.getReference (0) = in;
        if (layout.outputBuses.size() > 0) layout.outputBuses.getReference (0) = out;

        return p.checkBusesLayoutSupported (layout) && p.setBusesLayout (layout);
    };

    const auto stereo = juce::AudioChannelSet::stereo();
    const auto mono   = juce::AudioChannelSet::mono();

    if (! tryLayout (stereo, stereo))
        if (! tryLayout (mono, stereo))
            tryLayout (mono, mono);

    p.setProcessingPrecision (juce::AudioProcessor::singlePrecision);
    p.setRateAndBufferSizeDetails (sr, bs);
    p.prepareToPlay (sr, bs);
}

static void updateSlotChannels (Rack::Slot& s)
{
    if (s.plugin == nullptr)
        return;

    const auto ins  = s.plugin->getTotalNumInputChannels();
    const auto outs = s.plugin->getTotalNumOutputChannels();

    s.numChannels = juce::jmax (ins, outs, 1);
    s.mono = (ins <= 1 && outs == 1);
}

juce::Optional<juce::AudioPlayHead::PositionInfo> Rack::TempoPlayHead::getPosition() const
{
    const rt::HostScope host;   // called by the plugin, but this code is ours
    if (rack.tempo == nullptr)
        return {};

    const auto& b = rack.tempo->getBlockInfo (rack.tempoIndex.load (std::memory_order_relaxed));
    PositionInfo info;
    info.setBpm (b.bpm);
    info.setTimeSignature (TimeSignature { 4, 4 });
    info.setPpqPosition (b.ppq);
    const auto bar = std::floor (b.ppq / 4.0);
    info.setPpqPositionOfLastBarStart (bar * 4.0);
    info.setBarCount ((juce::int64) bar);
    info.setTimeInSamples (b.samplePos);
    info.setTimeInSeconds ((double) b.samplePos / juce::jmax (1.0, rack.sampleRate));
    info.setIsPlaying (true);   // a live rig always runs: synced plugins work without a transport
    if (b.hostTimeNs != 0)
        info.setHostTimeNs (b.hostTimeNs);
    return info;
}

std::unique_ptr<juce::AudioPluginInstance> Rack::setPlugin (int slot, std::unique_ptr<juce::AudioPluginInstance> p)
{
    jassert (juce::isPositiveAndBelow (slot, maxSlots));

    if (p != nullptr)
        p->setPlayHead (&playHead);

    Slot temp;
    temp.au = auwg::Context (p.get());
    temp.plugin = std::move (p);
    updateSlotChannels (temp);

    {
        const juce::SpinLock::ScopedLockType sl (processLock);
        auto& s = slots[(size_t) slot];
        dropTwin (s);
        std::swap (s.plugin, temp.plugin);
        s.au.swap (temp.au);
        s.numChannels = temp.numChannels;
        s.mono = temp.mono;
        s.bypassed = false;
        s.cpuMs = 0.0f;
        s.hopCalls = 0;
        s.hopPhase = -1;
        s.heavyCount = 0;
        s.lightMs = 1.0e9f;
        s.hopWaitMs = -1.0f;
        s.wakeNeed = 0;
        s.wakeProbeMs = 0.0f;
        s.wakeProbeStreak = 0;
        s.bypassState = Slot::bpActive;
        s.flushWanted = false;
        s.flushedParams = 0;
        s.wet = 1.0f;
        s.drainLeft = s.drainQuiet = 0;
        s.owed = 0;
        s.ringPos = 0;
        s.bypassRing.clear();
        s.shownParams.clearQuick();
        s.paramsChosen = false;
        s.viewMode = defaultViewMode;
        s.hiddenMeters.clear();
    }

    if (temp.plugin != nullptr)
        temp.plugin->setPlayHead (nullptr);   // may be parked or moved: never points at a rack that goes away

    return std::move (temp.plugin);
}

void Rack::moveSlot (int from, int to)
{
    if (from == to || ! juce::isPositiveAndBelow (from, maxSlots) || ! juce::isPositiveAndBelow (to, maxSlots))
        return;

    const juce::SpinLock::ScopedLockType sl (processLock);
    auto& a = slots[(size_t) from];
    auto& b = slots[(size_t) to];

    dropTwin (a);   // a hand-over is short: simply restart it later if needed
    dropTwin (b);
    std::swap (a.plugin, b.plugin);
    a.au.swap (b.au);
    std::swap (a.numChannels, b.numChannels);
    std::swap (a.mono, b.mono);
    std::swap (a.shownParams, b.shownParams);
    std::swap (a.paramsChosen, b.paramsChosen);
    std::swap (a.uiSize, b.uiSize);
    std::swap (a.viewMode, b.viewMode);
    std::swap (a.hiddenMeters, b.hiddenMeters);
    const int ah = a.hopCalls, ap = a.hopPhase;
    a.hopCalls = b.hopCalls.load();
    a.hopPhase = b.hopPhase.load();
    b.hopCalls = ah;
    b.hopPhase = ap;

    const int an = a.wakeNeed;
    a.wakeNeed = b.wakeNeed.load();
    b.wakeNeed = an;
    std::swap (a.wakeProbeMs, b.wakeProbeMs);
    std::swap (a.wakeProbeStreak, b.wakeProbeStreak);

    const bool ab = a.bypassed.load();
    a.bypassed = b.bypassed.load();
    b.bypassed = ab;

    const int abs = a.bypassState;
    a.bypassState = b.bypassState.load();
    b.bypassState = abs;
    a.flushWanted = false;  b.flushWanted = false;
    a.flushedParams = 0;    b.flushedParams = 0;
    std::swap (a.wet, b.wet);
    std::swap (a.drainLeft, b.drainLeft);
    std::swap (a.drainQuiet, b.drainQuiet);
    std::swap (a.owed, b.owed);
    std::swap (a.bypassRing, b.bypassRing);
    std::swap (a.ringPos, b.ringPos);
}

//==============================================================================
void Rack::dropTwin (Slot& s)
{
    if (s.twin != nullptr)
        discarded.push_back (std::move (s.twin));   // deleted later, outside the lock

    s.handoverDone = false;
    s.twinPos = 0;
}

bool Rack::beginHandover (int slot, std::unique_ptr<juce::AudioPluginInstance>& twin, int hopCalls, int hopPhase, juce::uint64 firstCallback,
                          int warmSamples, int fadeSamples)
{
    if (! juce::isPositiveAndBelow (slot, maxSlots) || twin == nullptr)
        return false;

    auwg::Context twinAu (twin.get());   // reads an AU property: not under the lock
    const juce::SpinLock::ScopedLockType sl (processLock);
    auto& s = slots[(size_t) slot];

    if (s.plugin == nullptr || s.twin != nullptr
        || twin->getTotalNumInputChannels() != s.plugin->getTotalNumInputChannels()
        || twin->getTotalNumOutputChannels() != s.plugin->getTotalNumOutputChannels())
        return false;

    twin->setPlayHead (&playHead);
    s.twinHopCalls = hopCalls;
    s.twinHopPhase = hopPhase;
    s.twinWarmSamples = juce::jmax (0, warmSamples);
    s.twinFadeSamples = juce::jmax (1, fadeSamples);
    s.twinPos = 0;
    s.twinFirstCallback = firstCallback;
    s.handoverDone = false;
    s.auTwin.swap (twinAu);   // the old one is released after the lock
    s.twin = std::move (twin);
    return true;
}

bool Rack::isHandingOver (int slot) const noexcept
{
    return juce::isPositiveAndBelow (slot, maxSlots) && slots[(size_t) slot].twin != nullptr;
}

std::unique_ptr<juce::AudioPluginInstance> Rack::collectHandover (int slot)
{
    if (! juce::isPositiveAndBelow (slot, maxSlots))
        return {};

    auto& s = slots[(size_t) slot];

    if (! s.handoverDone.load (std::memory_order_acquire))
        return {};

    std::unique_ptr<juce::AudioPluginInstance> retired;
    {
        const juce::SpinLock::ScopedLockType sl (processLock);
        retired = std::move (s.twin);
        s.handoverDone = false;
        s.twinPos = 0;
    }
    if (retired != nullptr)
        retired->setPlayHead (nullptr);
    return retired;
}

std::vector<std::unique_ptr<juce::AudioPluginInstance>> Rack::takeDiscarded()
{
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> out;
    {
        const juce::SpinLock::ScopedLockType sl (processLock);
        std::swap (out, discarded);
    }
    for (auto& p : out)
        if (p != nullptr) p->setPlayHead (nullptr);
    return out;
}

juce::AudioPluginInstance* Rack::getPlugin (int slot) const noexcept
{
    return juce::isPositiveAndBelow (slot, maxSlots) ? slots[(size_t) slot].plugin.get() : nullptr;
}

int Rack::getNumPlugins() const noexcept
{
    int n = 0;
    for (auto& s : slots)
        n += (s.plugin != nullptr) ? 1 : 0;
    return n;
}

int Rack::getLatencySamples() const noexcept
{
    int total = 0;
    for (auto& s : slots)
        if (s.plugin != nullptr)
            total += s.plugin->getLatencySamples();
    return total;
}

//==============================================================================
void Rack::prepare (double sr, int devBlock)
{
    {
        const juce::SpinLock::ScopedLockType sl (processLock);

        sampleRate = sr;
        deviceBlockSize = devBlock;
        pluginBlockSize = devBlock;

        buffer.setSize (maxPluginChannels, juce::jmax (deviceBlockSize, pluginBlockSize), false, true, false);
        buffer.clear();
        preBuffer.setSize (2, juce::jmax (deviceBlockSize, pluginBlockSize), false, true, false);
        preBuffer.clear();
        tempoTapBuffer.setSize (3, juce::jmax (deviceBlockSize, pluginBlockSize), false, true, false);
        tempoTapBuffer.clear();
        tempoTapValid = {};
        preValid = false;
        twinBuffer.setSize (maxPluginChannels, juce::jmax (deviceBlockSize, pluginBlockSize), false, true, false);
        twinBuffer.clear();
        dryBuffer.setSize (maxPluginChannels, juce::jmax (deviceBlockSize, pluginBlockSize), false, true, false);
        dryBuffer.clear();
        midi.ensureSize (2048);
        gain.reset (sr, 0.02);
        gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainDb.load()));
        inGain.reset (sr, 0.02);
        inGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inputGainDb.load()));

        // automix level detector (see updateAutomixLevel)
        amHighPass.set (juce::IIRCoefficients::makeHighPass (sr, automixHighPassHz.load()));
        amLowPass.set (juce::IIRCoefficients::makeLowPass (sr, juce::jmin ((double) automixLowPassHz.load(), sr * 0.45)));
        amAvgCoeff = (float) std::exp (-1.0 / (automixAvgMs.load() * 0.001 * sr));   // per sample
        amHoldSamples = (int) (automixHoldMs.load() * 0.001 * sr);
        amReleaseDbPerSample = (float) (automixReleaseDbPerSec.load() / sr);
        amFloorRisePerSample = (float) (3.0 / sr);   // 3 dB/s, so a talker never turns into the floor
        amActive = false;

        for (auto& s : slots)
        {
            dropTwin (s);   // prepared for the old rate/block

            if (s.plugin != nullptr)
            {
                s.plugin->releaseResources();
                configurePlugin (*s.plugin, sr, pluginBlockSize);
                updateSlotChannels (s);
                s.hopCalls = 0;   // prepareToPlay reset its frame phase: the engine re-aligns
                s.hopPhase = -1;
            }

            s.owed = 0;   // calls counted against the old phase
            s.bypassRing.setSize (2, Slot::bypassRingSize, false, true, false);
            s.bypassRing.clear();
            s.ringPos = 0;
        }
    }
}

void Rack::release()
{
    const juce::SpinLock::ScopedLockType sl (processLock);

    for (auto& s : slots)
        if (s.plugin != nullptr)
            s.plugin->releaseResources();
}

static double threadCpuMs() noexcept
{
    timespec ts {};
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1.0e6;
}

//==============================================================================
void Rack::callPlugin (Slot& s, juce::AudioPluginInstance& inst, juce::AudioBuffer<float>& b, bool bypassed) noexcept
{
    midi.clear();
    const juce::ScopedLock cl (inst.getCallbackLock());
    const rt::PluginScope inPlugin;   // what the plugin allocates is not ours

    if (auwg::enabled.load (std::memory_order_relaxed))
    {
        auto* wg = auwg::current.load (std::memory_order_relaxed);
        s.au.notify (inst, wg);       // each checks it belongs to `inst`
        s.auTwin.notify (inst, wg);
    }

    if (inst.isSuspended())
    {
        // leave the signal untouched
    }
    else if (bypassed)
    {
        inst.processBlockBypassed (b, midi);
    }
    else
    {
        inst.processBlock (b, midi);
    }
}

void Rack::readDelayedDry (const Slot& s, juce::AudioBuffer<float>& dest, int numSamples, int latency) noexcept
{
    // the ring already holds this callback's input (processSlot), ringPos is past it
    constexpr int mask = Slot::bypassRingSize - 1;
    const int start = s.ringPos - numSamples - latency;

    for (int ch = 0; ch < dest.getNumChannels(); ++ch)
    {
        const auto* r = s.bypassRing.getReadPointer (juce::jmin (ch, s.bypassRing.getNumChannels() - 1));
        auto* d = dest.getWritePointer (ch);

        for (int i = 0; i < numSamples; ++i)
            d[i] = r[(start + i) & mask];
    }
}

void Rack::processBypassTransition (Slot& s, juce::AudioBuffer<float>& view, int numSamples,
                                    bool trueBypass, bool bypassedNow, int latency) noexcept
{
    // Bypass on:  crossfade to dry (10 ms) -> drain: silence in until its output stays quiet
    //             (reverb/delay tails die out inside, so nothing stale comes back) -> idle,
    //             not called at all.
    // Bypass off: if it was idle, stay dry until the missed calls are a whole number of hops (one
    //             hop at most, 4 ms for De-Feedback), so its hop is on the phase the spread gave
    //             it; then crossfade back in. No extra calls: they put two calls (and a hop out
    //             of its phase) in one callback.
    auto& p = *s.plugin;
    const int numCh = view.getNumChannels();
    juce::AudioBuffer<float> scratch (dryBuffer.getArrayOfWritePointers(), numCh, numSamples);

    if (trueBypass && s.wet <= 0.0f)
    {
        if (s.drainLeft > 0)
        {
            scratch.clear();
            callPlugin (s, p, scratch, false);

            float peak = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
                peak = juce::jmax (peak, scratch.getMagnitude (ch, 0, numSamples));

            s.drainQuiet = peak < 1.0e-5f ? s.drainQuiet + numSamples : 0;   // -100 dBFS
            s.drainLeft -= numSamples;

            if (s.drainQuiet >= (int) (sampleRate * 0.3))
                s.drainLeft = 0;
        }
        else
        {
            // Session::flushIdlePlugins: one silent call delivers the parameter changes made while
            // idle (VST3/CLAP take them only in process). In phase with the spread, so it
            // counts as the call of this callback and the hop stays where it was.
            const auto hop = s.hopCalls.load (std::memory_order_relaxed);

            if (s.flushWanted.load (std::memory_order_acquire) && (hop <= 1 || s.owed % hop == 0))
            {
                scratch.clear();
                callPlugin (s, p, scratch, false);
                s.flushWanted.store (false, std::memory_order_release);
            }
            else
            {
                ++s.owed;
            }

            s.bypassState.store (Slot::bpIdle, std::memory_order_relaxed);
        }

        if (latency > 0)
            readDelayedDry (s, view, numSamples, latency);   // dry, as late as the plugin would be
        return;   // dry: the signal is untouched
    }

    s.bypassState.store (Slot::bpTransition, std::memory_order_relaxed);
    s.flushedParams.store (0, std::memory_order_relaxed);

    if (! trueBypass && s.wet <= 0.0f && s.owed > 0)
    {
        const auto hop = s.hopCalls.load (std::memory_order_relaxed);

        if (hop > 1 && s.owed % hop != 0)
        {
            ++s.owed;                                           // still dry: its turn comes within a hop
            if (latency > 0)
                readDelayedDry (s, view, numSamples, latency);
            return;
        }

        s.owed = 0;
    }

    s.drainLeft = s.drainQuiet = 0;

    if (latency > 0)
        readDelayedDry (s, scratch, numSamples, latency);   // lined up with the plugin's output
    else
        for (int ch = 0; ch < numCh; ++ch)
            scratch.copyFrom (ch, 0, view, ch, 0, numSamples);

    callPlugin (s, p, view, bypassedNow && ! trueBypass);

    // equal-gain crossfade (dry and processed are highly correlated)
    const float step = (trueBypass ? -1.0f : 1.0f) / (float) juce::jmax (1.0, sampleRate * 0.01);
    float w = s.wet;

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* d = view.getWritePointer (ch);
        const auto* dry = scratch.getReadPointer (ch);
        w = s.wet;

        for (int i = 0; i < numSamples; ++i)
        {
            w = juce::jlimit (0.0f, 1.0f, w + step);
            d[i] = dry[i] + w * (d[i] - dry[i]);
        }
    }

    s.wet = w;

    if (s.wet <= 0.0f)
    {
        s.drainLeft = (int) (sampleRate * 10.0);   // noisy plugins (analog models) never go quiet
        s.drainQuiet = 0;
    }
    else if (s.wet >= 1.0f)
    {
        s.bypassState.store (Slot::bpActive, std::memory_order_relaxed);
    }
}

void Rack::processSlot (Slot& s, juce::AudioBuffer<float>& buf, int numSamples) noexcept
{
    auto& p = *s.plugin;
    const auto t0 = juce::Time::getHighResolutionTicks();
    const int n = juce::jmin (s.numChannels, maxPluginChannels);

    // Extra channels (beyond the stereo rack bus) must not carry stale audio.
    for (int ch = 2; ch < n; ++ch)
        buf.clear (ch, 0, numSamples);

    const int viewChannels = s.mono ? 1 : n;
    juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), viewChannels, numSamples);
    const bool bypassedNow = s.bypassed.load (std::memory_order_relaxed);
    auto run = [&] (juce::AudioPluginInstance& inst, juce::AudioBuffer<float>& b) { callPlugin (s, inst, b, bypassedNow); };

    // True bypass (see Slot::bypassState). Plugins with latency keep their own bypass, which
    // delays the dry signal as the latency compensation expects; so does a slot handing over.
    int latency = p.getLatencySamples();

    if (latency > 0)
    {
        if (latency + numSamples < Slot::bypassRingSize && viewChannels <= s.bypassRing.getNumChannels()
            && s.bypassRing.getNumSamples() == Slot::bypassRingSize)
        {
            constexpr int mask = Slot::bypassRingSize - 1;

            for (int ch = 0; ch < s.bypassRing.getNumChannels(); ++ch)
            {
                const auto* x = view.getReadPointer (juce::jmin (ch, viewChannels - 1));
                auto* r = s.bypassRing.getWritePointer (ch);

                for (int i = 0; i < numSamples; ++i)
                    r[(s.ringPos + i) & mask] = x[i];
            }

            s.ringPos = (s.ringPos + numSamples) & mask;
        }
        else
        {
            latency = -1;   // no room to delay the dry signal: the plugin's own bypass
        }
    }

    const bool trueBypass = bypassedNow && features::has (features::trueBypass) && latency >= 0 && s.twin == nullptr;

    if ((trueBypass || s.wet < 1.0f) && numSamples <= dryBuffer.getNumSamples() && latency >= 0)
    {
        processBypassTransition (s, view, numSamples, trueBypass, bypassedNow, latency);

        if (s.mono)
            buf.copyFrom (1, 0, buf, 0, 0, numSamples);

        s.cpuMs.store ((float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0),
                       std::memory_order_relaxed);
        return;
    }

    if (s.bypassState.load (std::memory_order_relaxed) != Slot::bpActive)
        s.bypassState.store (Slot::bpActive, std::memory_order_relaxed);

    // Hand-over in progress: the twin gets the very same input.
    const bool handover = s.twin != nullptr && ! s.handoverDone.load (std::memory_order_relaxed)
                          && numSamples <= twinBuffer.getNumSamples();

    if (handover && s.twinPos == 0)
    {
        // Started k callbacks later than its alignment assumed: k silent blocks put its hop
        // back on the chosen phase (its output is not heard yet: warm-up).
        const auto late = (juce::int64) callbackIndex - (juce::int64) s.twinFirstCallback;
        s.twinLateBy.store ((int) late, std::memory_order_relaxed);

        if (s.twinHopCalls > 1 && late > 0)
        {
            juce::AudioBuffer<float> silent (twinBuffer.getArrayOfWritePointers(), viewChannels, numSamples);

            for (int k = 0; k < (int) (late % s.twinHopCalls); ++k)
            {
                silent.clear();
                run (*s.twin, silent);
            }
        }
    }

    if (handover)
        for (int ch = 0; ch < viewChannels; ++ch)
            twinBuffer.copyFrom (ch, 0, buf, ch, 0, numSamples);

    // on the expected hop call, also measure the thread's own CPU time (see hopWaitMs)
    const auto hop = s.hopCalls.load (std::memory_order_relaxed);
    const bool hopCall = hop > 1 && (int) (callbackIndex % (juce::uint64) hop) == s.hopPhase.load (std::memory_order_relaxed);
    const auto cpu0 = hopCall ? threadCpuMs() : 0.0;
    const auto tp = juce::Time::getHighResolutionTicks();

    run (p, view);
    float probeWait = 0.0f;   // a hop that is not heavy did not wait

    // hop watch: heavy = well above an ordinary call (same rule as HopAligner)
    const auto ms = (float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - tp) * 1000.0);

    if (ms > juce::jmax (s.lightMs * 4.0f, s.lightMs + 0.03f))
    {
        if (hopCall)
        {
            const auto wait = juce::jmax (0.0f, ms - (float) (threadCpuMs() - cpu0));
            const auto old = s.hopWaitMs.load (std::memory_order_relaxed);
            s.hopWaitMs.store (old < 0.0f ? wait : old + 0.05f * (wait - old), std::memory_order_relaxed);

            probeWait = wait;
        }

        const auto n = s.heavyCount.load (std::memory_order_relaxed);
        s.heavyAt[n % Slot::heavyRing].store (callbackIndex, std::memory_order_relaxed);
        s.heavyCount.store (n + 1, std::memory_order_release);
        s.lightMs += 0.0005f * (ms - s.lightMs);
    }
    else
    {
        s.lightMs = ms < s.lightMs ? ms : s.lightMs + 0.002f * (ms - s.lightMs);
    }

    // unassisted hop after an idle gap: long waits in several probes in a row mean the
    // assist helps (for good); a fast probe restarts the count
    if (hopCall && wakeProbeNow.load (std::memory_order_relaxed))
    {
        s.wakeProbeMs = juce::jmax (s.wakeProbeMs, probeWait);
        s.wakeProbeStreak = probeWait > wakeNeedMs ? s.wakeProbeStreak + 1 : 0;

        if (s.wakeProbeStreak >= wakeNeedProbes)
            s.wakeNeed.store (1, std::memory_order_relaxed);
    }

    if (handover)
    {
        juce::AudioBuffer<float> twinView (twinBuffer.getArrayOfWritePointers(), viewChannels, numSamples);

        run (*s.twin, twinView);

        // Equal-gain crossfade (the two outputs are near-identical, i.e. highly correlated).
        const int fadeStart = s.twinWarmSamples, fadeLen = s.twinFadeSamples;

        if (s.twinPos + numSamples > fadeStart)
        {
            for (int ch = 0; ch < viewChannels; ++ch)
            {
                auto* d = view.getWritePointer (ch);
                const auto* t = twinView.getReadPointer (ch);

                for (int i = 0; i < numSamples; ++i)
                {
                    const auto g = juce::jlimit (0.0f, 1.0f, (float) (s.twinPos + i + 1 - fadeStart) / (float) fadeLen);
                    d[i] += g * (t[i] - d[i]);
                }
            }
        }

        s.twinPos += numSamples;

        if (s.twinPos >= fadeStart + fadeLen)
        {
            // the twin is now the slot's plugin; the old instance waits in `twin` for collectHandover()
            std::swap (s.plugin, s.twin);
            s.au.swap (s.auTwin);
            s.hopCalls.store (s.twinHopCalls, std::memory_order_relaxed);
            s.hopPhase.store (s.twinHopPhase, std::memory_order_relaxed);
            s.handoverDone.store (true, std::memory_order_release);
        }
    }

    if (s.mono)
        buf.copyFrom (1, 0, buf, 0, 0, numSamples);

    s.cpuMs.store ((float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0),
                   std::memory_order_relaxed);
}


void Rack::runChain (juce::AudioBuffer<float>& buf, int numSamples) noexcept
{
    const auto t0 = juce::Time::getHighResolutionTicks();
    const bool measureCpu = measureCpuTime.load (std::memory_order_relaxed);   // a syscall: only for --bench
    const auto c0 = measureCpu ? threadCpuMs() : 0.0;

    if (! bypassed.load (std::memory_order_relaxed))
        for (auto& s : slots)
            if (s.plugin != nullptr)
                processSlot (s, buf, numSamples);

    const auto ms = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0;
    cpuMs.store ((float) ms, std::memory_order_relaxed);

    if (measureCpu)
        cpuTimeMs.store ((float) (threadCpuMs() - c0), std::memory_order_relaxed);

    size_t cpu = 0;
    if (pthread_cpu_number_np (&cpu) == 0)
        lastCpu.store ((int) cpu, std::memory_order_relaxed);
    const auto load = (float) (ms / (1000.0 * numSamples / sampleRate));
    cpuLoad.store (load, std::memory_order_relaxed);
    peakLoad.push (load);
}

void Rack::applyCompensation (int numSamples) noexcept
{
    const int delay = juce::jlimit (0, maxCompensation - 1, compensationSamples.load (std::memory_order_relaxed));

    if (delay == 0)
        return;

    constexpr int mask = maxCompensation - 1;

    for (int ch = 0; ch < 2; ++ch)
    {
        auto* io = buffer.getWritePointer (ch);
        auto* ring = delayLine.getWritePointer (ch);
        int w = delayWrite;

        for (int i = 0; i < numSamples; ++i, w = (w + 1) & mask)
        {
            ring[w] = io[i];
            io[i] = ring[(w - delay) & mask];
        }
    }

    delayWrite = (delayWrite + numSamples) & mask;
}

void Rack::updateAutomixLevel (int numSamples) noexcept
{
    // automixer switched off in SETTINGS > Features: no detector at all (no cost)
    if (automixGroup.load (std::memory_order_relaxed) <= 0 || muted.load (std::memory_order_relaxed)
        || ! features::has (features::automix))
    {
        // out of the group at once (a muted rack too)
        automixLevel = 0.0f;
        automixTalk = 0.0f;
        amActive = false;
        automixLevelDb.store (-100.0f, std::memory_order_relaxed);
        return;
    }

    if (! amActive)
    {
        amHighPass.reset(); amLowPass.reset();
        amAvg = 0.0f; amEnvDb = -200.0f; amHoldLeft = 0; amFloorDb = -200.0f;
        amActive = true;
    }

    // Detector, after the fader. 1) voice band only (~200 Hz-4 kHz): bass bleed from the stage
    // or a hum can't take the floor, and low notes don't modulate the gain. 2) power averaged over
    // ~5 ms. 3) dB envelope: rises at once (no clipped first syllable), holds 40 ms (the gaps
    // inside words), then falls at 100 dB/s, so a mic that stopped talking hands over quickly.
    const auto* l = buffer.getReadPointer (0);
    const auto* r = buffer.getReadPointer (1);
    float avg = amAvg;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto x = amLowPass.process (amHighPass.process (0.5f * (l[i] + r[i])));
        avg = x * x + amAvgCoeff * (avg - x * x);
    }

    amAvg = avg < 1.0e-20f ? 0.0f : avg;   // no denormals after a long silence
    const auto db = 10.0f * std::log10 (juce::jmax (amAvg, 1.0e-20f));

    if (db >= amEnvDb)
    {
        amEnvDb = db;
        amHoldLeft = amHoldSamples;
    }
    else if (amHoldLeft > 0)
    {
        amHoldLeft -= numSamples;
    }
    else
    {
        amEnvDb = juce::jmax (db, amEnvDb - amReleaseDbPerSample * (float) numSamples);
    }

    automixLevel = amEnvDb < -190.0f ? 0.0f : std::pow (10.0f, 0.1f * amEnvDb);

    // own noise floor (minimum statistics) -> is someone talking into this mic? Only used by
    // the Soft/Open overlap modes, never by the Dugan law itself.
    if (amFloorDb < -190.0f || db < amFloorDb)
        amFloorDb = db;
    else
        amFloorDb += amFloorRisePerSample * (float) numSamples;

    automixTalk = juce::jlimit (0.0f, 1.0f, (amEnvDb - amFloorDb - 6.0f) / 6.0f);
    automixLevelDb.store (juce::jmax (-100.0f, amEnvDb), std::memory_order_relaxed);
}

void Rack::sumFeeds (int numSamples) noexcept
{
    buffer.clear (0, 0, numSamples);
    buffer.clear (1, 0, numSamples);

    for (auto& f : feeds)
    {
        if (f.kind == 0)
            continue;

        auto& s = *f.src;

        if (! s.producedAudio() || ! s.passesSolo())
        {
            f.gain = 0.0f;   // nothing (valid) to read: no ramp from stale data
            continue;
        }

        const juce::AudioBuffer<float>* src = &s.getOutput();
        float target = 0.0f;

        if (f.kind == 1)
        {
            target = s.automixGain;   // routed: everything (fader, mute, automix)
        }
        else if (auto& send = s.sends[(size_t) f.send]; send.isOn())
        {
            target = juce::Decibels::decibelsToGain (send.db.load (std::memory_order_relaxed), sendOffDb);

            if (send.pre.load (std::memory_order_relaxed))
            {
                // pre-fader: before gain and automix, but a muted rack sends nothing
                if (s.hasPreFader())
                    src = &s.getPreFader();
                else
                    target = 0.0f;

                if (s.muted.load (std::memory_order_relaxed))
                    target = 0.0f;
            }
            else
            {
                target *= s.automixGain;   // post-fader: what the rack plays, automix included
            }
        }

        const auto g0 = f.gain;
        f.gain = target;

        if (g0 == 0.0f && target == 0.0f)
            continue;

        for (int ch = 0; ch < 2; ++ch)
        {
            if (g0 == target)
                buffer.addFrom (ch, 0, *src, ch, 0, numSamples, target);
            else
                buffer.addFromWithRamp (ch, 0, src->getReadPointer (ch), numSamples, g0, target);
        }
    }
}

// Inf / NaN from a device or a plugin: silence the block, so it never reaches the speakers, the
// buses, the delay lines or the meters (an infinite meter value would never come down).
// Returns false if the block was cleared.
static bool keepFinite (juce::AudioBuffer<float>& b, int numSamples) noexcept
{
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (b.getReadPointer (ch), numSamples);

        if (! (std::isfinite (range.getStart()) && std::isfinite (range.getEnd())))
        {
            b.clear (0, 0, numSamples);
            b.clear (1, 0, numSamples);
            return false;
        }
    }

    return true;
}

void Rack::captureTempoTap (int point, int numSamples) noexcept
{
    if ((tempoTapMask.load (std::memory_order_relaxed) & (1 << point)) == 0 || numSamples > tempoTapBuffer.getNumSamples())
        return;

    auto* d = tempoTapBuffer.getWritePointer (point - 1);
    juce::FloatVectorOperations::copyWithMultiply (d, buffer.getReadPointer (0), 0.5f, numSamples);
    juce::FloatVectorOperations::addWithMultiply (d, buffer.getReadPointer (1), 0.5f, numSamples);
    tempoTapValid[(size_t) point] = true;
}

void Rack::process (const float* const* deviceInputs, int numDeviceInputs, int numSamples, int threadIndex) noexcept
{
    const rt::HostScope host;
    const juce::ScopedNoDenormals noDenormals;
    const auto t0 = juce::Time::getHighResolutionTicks();
    hasAudio = false;
    tempoTapValid = {};

    auto readInput = [&]
    {
        const int inCh = inputChannel.load (std::memory_order_relaxed);
        const bool stereoIn = stereoInput.load (std::memory_order_relaxed);

        auto copyIn = [&] (int dest, int src)
        {
            if (juce::isPositiveAndBelow (src, numDeviceInputs) && deviceInputs[src] != nullptr)
                buffer.copyFrom (dest, 0, deviceInputs[src], numSamples);
            else
                buffer.clear (dest, 0, numSamples);
        };

        if (isActiveBus)
        {
            sumFeeds (numSamples);   // the racks routed / sending here ran in earlier stages
        }
        else if (sourceRack != nullptr)
        {
            // chained: the source ran in an earlier stage of this same callback
            if (sourceRack->producedAudio())
            {
                buffer.copyFrom (0, 0, sourceRack->getOutput(), 0, 0, numSamples);
                buffer.copyFrom (1, 0, sourceRack->getOutput(), stereoIn ? 1 : 0, 0, numSamples);
            }
            else
            {
                buffer.clear (0, 0, numSamples);
                buffer.clear (1, 0, numSamples);
            }
        }
        else if (inCh < 0)
        {
            buffer.clear (0, 0, numSamples);
            buffer.clear (1, 0, numSamples);
        }
        else
        {
            copyIn (0, inCh);

            // stereo input whose second channel the device does not have (e.g. the Mac's mono
            // microphone on "IN 1-2"): both sides get the first one rather than one side silent
            if (const int right = stereoIn ? inCh + 1 : inCh;
                stereoIn && ! (juce::isPositiveAndBelow (right, numDeviceInputs) && deviceInputs[right] != nullptr))
                buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);
            else
                copyIn (1, right);
        }

        // input trim (before the plugins, so the meter shows what they get)
        inGain.setTargetValue (juce::Decibels::decibelsToGain (inputGainDb.load (std::memory_order_relaxed), -100.0f));

        if (inGain.isSmoothing())
        {
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto g = inGain.getNextValue();
                l[i] *= g;
                r[i] *= g;
            }
        }
        else if (const auto g = inGain.getTargetValue(); g != 1.0f)
        {
            buffer.applyGain (0, 0, numSamples, g);
            buffer.applyGain (1, 0, numSamples, g);
        }

        if (! keepFinite (buffer, numSamples))
            nonFiniteBlocks.fetch_add (1, std::memory_order_relaxed);

        inMeter[0].push (buffer.getMagnitude (0, 0, numSamples));
        inMeter[1].push (buffer.getMagnitude (1, 0, numSamples));
        captureTempoTap (1, numSamples);   // tempo taps: before the plugins
    };

    auto applyGainAndMeter = [&]
    {
        if (! keepFinite (buffer, numSamples))   // a plugin blew up
            nonFiniteBlocks.fetch_add (1, std::memory_order_relaxed);

        captureTempoTap (2, numSamples);   // tempo taps: after the plugins, before the fader

        // latency-group delay first, so pre- and post-fader sends are both compensated
        applyCompensation (numSamples);

        // pre-fader copy, only while a pre-fader send is on
        preValid = false;

        if (const auto allowed = sendAllowed.load (std::memory_order_relaxed); allowed != 0)
        {
            for (int k = 0; k < maxBuses; ++k)
            {
                if ((allowed & (1u << k)) != 0 && sends[(size_t) k].pre.load (std::memory_order_relaxed) && sends[(size_t) k].isOn())
                {
                    preBuffer.copyFrom (0, 0, buffer, 0, 0, numSamples);
                    preBuffer.copyFrom (1, 0, buffer, 1, 0, numSamples);
                    preValid = true;
                    break;
                }
            }
        }

        const float target = muted.load (std::memory_order_relaxed)
                                 ? 0.0f
                                 : juce::Decibels::decibelsToGain (gainDb.load (std::memory_order_relaxed), -100.0f);
        gain.setTargetValue (target);

        if (gain.isSmoothing())
        {
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto g = gain.getNextValue();
                l[i] *= g;
                r[i] *= g;
            }
        }
        else if (target != 1.0f)
        {
            buffer.applyGain (0, 0, numSamples, target);
            buffer.applyGain (1, 0, numSamples, target);
        }

        // automix detector AFTER the fader, like Dugan Speech in Waves consoles: pulling a
        // fader down also takes that mic's claim on the floor (the way to quiet a noisy mic)
        updateAutomixLevel (numSamples);
        captureTempoTap (3, numSamples);   // tempo taps: after the fader (and mute)

        outMeter[0].push (buffer.getMagnitude (0, 0, numSamples));
        outMeter[1].push (buffer.getMagnitude (1, 0, numSamples));
        hasAudio = true;
    };

    {
        // the message thread holds this only to swap a plugin / twin in or out
        const bool locked = rt::enterWithin (processLock, 40.0);
        const rt::ScopedExit unlock { locked ? &processLock : nullptr };

        if (! locked || numSamples > buffer.getNumSamples())
            return;

        readInput();
        runChain (buffer, numSamples);
        applyGainAndMeter();
        lastThread.store (threadIndex, std::memory_order_relaxed);
    }

    callbackMs.store ((float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0),
                      std::memory_order_relaxed);
}

} // namespace stagerack
