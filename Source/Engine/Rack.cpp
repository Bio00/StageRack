#include "Rack.h"
#include "WorkerPool.h"
#include <pthread.h>
#include <ctime>

namespace stagerack
{

//==============================================================================
/** Realtime thread that runs an async rack's chain whenever a full internal block is buffered. */
class Rack::AsyncWorker final : public juce::Thread
{
public:
    explicit AsyncWorker (Rack& r) : juce::Thread ("StageRack async rack " + juce::String (r.getId())), rack (r) {}

    void wake() noexcept
    {
        signal.fetch_add (1, std::memory_order_release);
        signal.notify_one();
    }

    void requestQuit()
    {
        signalThreadShouldExit();
        wake();
    }

    void run() override
    {
        uint32_t seen = signal.load (std::memory_order_acquire);

        while (! threadShouldExit())
        {
            while (rack.inFifo.getNumReady() >= rack.pluginBlockSize && ! threadShouldExit())
                rack.processAsyncBlock();

            signal.wait (seen, std::memory_order_acquire);
            seen = signal.load (std::memory_order_acquire);
        }
    }

private:
    Rack& rack;
    std::atomic<uint32_t> signal { 0 };
};

//==============================================================================
Rack::Rack (int uniqueId) : id (uniqueId)
{
    delayLine.setSize (2, maxCompensation);
    delayLine.clear();
    buffer.setSize (maxPluginChannels, deviceBlockSize);
    gain.reset (sampleRate, 0.02);
}

Rack::~Rack()
{
    stopAsync();
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

std::unique_ptr<juce::AudioPluginInstance> Rack::setPlugin (int slot, std::unique_ptr<juce::AudioPluginInstance> p)
{
    jassert (juce::isPositiveAndBelow (slot, maxSlots));

    Slot temp;
    temp.plugin = std::move (p);
    updateSlotChannels (temp);

    {
        const juce::SpinLock::ScopedLockType sl (processLock);
        auto& s = slots[(size_t) slot];
        dropTwin (s);
        std::swap (s.plugin, temp.plugin);
        s.numChannels = temp.numChannels;
        s.mono = temp.mono;
        s.bypassed = false;
        s.cpuMs = 0.0f;
        s.hopCalls = 0;
        s.hopPhase = -1;
        s.shownParams.clearQuick();
        s.paramsChosen = false;
        s.viewMode = 0;
    }

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
    std::swap (a.numChannels, b.numChannels);
    std::swap (a.mono, b.mono);
    std::swap (a.shownParams, b.shownParams);
    std::swap (a.paramsChosen, b.paramsChosen);
    std::swap (a.uiSize, b.uiSize);
    std::swap (a.viewMode, b.viewMode);
    const int ah = a.hopCalls, ap = a.hopPhase;
    a.hopCalls = b.hopCalls.load();
    a.hopPhase = b.hopPhase.load();
    b.hopCalls = ah;
    b.hopPhase = ap;

    const bool ab = a.bypassed.load();
    a.bypassed = b.bypassed.load();
    b.bypassed = ab;
}

//==============================================================================
void Rack::dropTwin (Slot& s)
{
    if (s.twin != nullptr)
        discarded.push_back (std::move (s.twin));   // deleted later, outside the lock

    s.handoverDone = false;
    s.twinPos = 0;
}

bool Rack::beginHandover (int slot, std::unique_ptr<juce::AudioPluginInstance>& twin, int hopCalls, int hopPhase,
                          int warmSamples, int fadeSamples)
{
    if (! juce::isPositiveAndBelow (slot, maxSlots) || twin == nullptr || isAsync())
        return false;

    const juce::SpinLock::ScopedLockType sl (processLock);
    auto& s = slots[(size_t) slot];

    if (s.plugin == nullptr || s.twin != nullptr
        || twin->getTotalNumInputChannels() != s.plugin->getTotalNumInputChannels()
        || twin->getTotalNumOutputChannels() != s.plugin->getTotalNumOutputChannels())
        return false;

    s.twinHopCalls = hopCalls;
    s.twinHopPhase = hopPhase;
    s.twinWarmSamples = juce::jmax (0, warmSamples);
    s.twinFadeSamples = juce::jmax (1, fadeSamples);
    s.twinPos = 0;
    s.handoverDone = false;
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
    return retired;
}

std::vector<std::unique_ptr<juce::AudioPluginInstance>> Rack::takeDiscarded()
{
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> out;
    const juce::SpinLock::ScopedLockType sl (processLock);
    std::swap (out, discarded);
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

int Rack::getAddedLatencySamples() const noexcept
{
    return isAsync() ? asyncPrefill : 0;
}

int Rack::getLatencySamples() const noexcept
{
    int total = getAddedLatencySamples();
    for (auto& s : slots)
        if (s.plugin != nullptr)
            total += s.plugin->getLatencySamples();
    return total;
}

//==============================================================================
void Rack::prepare (double sr, int devBlock)
{
    stopAsync();

    {
        const juce::SpinLock::ScopedLockType sl (processLock);

        sampleRate = sr;
        deviceBlockSize = devBlock;
        pluginBlockSize = asyncFactor >= 2 ? devBlock * asyncFactor : devBlock;

        buffer.setSize (maxPluginChannels, juce::jmax (deviceBlockSize, pluginBlockSize), false, true, false);
        buffer.clear();
        workBuffer.setSize (maxPluginChannels, pluginBlockSize, false, true, false);
        workBuffer.clear();
        twinBuffer.setSize (maxPluginChannels, juce::jmax (deviceBlockSize, pluginBlockSize), false, true, false);
        twinBuffer.clear();
        midi.ensureSize (2048);
        gain.reset (sr, 0.02);
        gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainDb.load()));

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
        }
    }

    startAsyncIfNeeded();
}

void Rack::release()
{
    stopAsync();

    const juce::SpinLock::ScopedLockType sl (processLock);

    for (auto& s : slots)
        if (s.plugin != nullptr)
            s.plugin->releaseResources();
}

void Rack::setAsyncFactor (int factor)
{
    asyncFactor = factor >= 2 ? juce::jmin (factor, 16) : 0;
    prepare (sampleRate, deviceBlockSize);
}

void Rack::stopAsync()
{
    asyncActive.store (false, std::memory_order_release);

    {
        // wait for the device thread to leave its async section
        const juce::SpinLock::ScopedLockType al (asyncLock);
    }

    if (asyncWorker != nullptr)
    {
        asyncWorker->requestQuit();
        asyncWorker->stopThread (2000);
        asyncWorker = nullptr;
    }
}

void Rack::startAsyncIfNeeded()
{
    if (asyncFactor < 2)
        return;

    const int b = pluginBlockSize, d = deviceBlockSize;
    asyncPrefill = b + 2 * d + juce::roundToInt (juce::jlimit (0.0f, 50.0f, asyncSafetyMs.load()) * sampleRate / 1000.0);
    const int capacity = 2 * asyncPrefill + 4 * b + 4 * d + 64;

    {
        const juce::SpinLock::ScopedLockType al (asyncLock);

        inFifo.setTotalSize (capacity);
        outFifo.setTotalSize (capacity);
        inRing.setSize (2, capacity, false, true, false);
        outRing.setSize (2, capacity, false, true, false);
        inRing.clear();
        outRing.clear();

        // Pre-roll of silence: gives the async thread a whole internal block of time
        // per block, whatever the alignment with the device callbacks.
        int s1, n1, s2, n2;
        outFifo.prepareToWrite (asyncPrefill, s1, n1, s2, n2);
        outFifo.finishedWrite (n1 + n2);
        xruns = 0;
    }

    asyncWorker = std::make_unique<AsyncWorker> (*this);

    if (! asyncWorker->startRealtimeThread (WorkerPool::realtimeOptionsFor (b, sampleRate)))
        asyncWorker->startThread (juce::Thread::Priority::highest);

    asyncActive.store (true, std::memory_order_release);
}

//==============================================================================
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

    auto run = [&] (juce::AudioPluginInstance& inst, juce::AudioBuffer<float>& b)
    {
        midi.clear();
        const juce::ScopedLock cl (inst.getCallbackLock());

        if (inst.isSuspended())
        {
            // leave the signal untouched
        }
        else if (bypassedNow)
        {
            inst.processBlockBypassed (b, midi);
        }
        else
        {
            inst.processBlock (b, midi);
        }
    };

    // Hand-over in progress: the twin gets the very same input.
    const bool handover = s.twin != nullptr && ! s.handoverDone.load (std::memory_order_relaxed)
                          && numSamples <= twinBuffer.getNumSamples();

    if (handover)
        for (int ch = 0; ch < viewChannels; ++ch)
            twinBuffer.copyFrom (ch, 0, buf, ch, 0, numSamples);

    run (p, view);

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

static double threadCpuMs() noexcept
{
    timespec ts {};
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1.0e6;
}

void Rack::runChain (juce::AudioBuffer<float>& buf, int numSamples) noexcept
{
    const auto t0 = juce::Time::getHighResolutionTicks();
    const auto c0 = threadCpuMs();

    if (! bypassed.load (std::memory_order_relaxed))
        for (auto& s : slots)
            if (s.plugin != nullptr)
                processSlot (s, buf, numSamples);

    const auto ms = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0;
    cpuMs.store ((float) ms, std::memory_order_relaxed);

    cpuTimeMs.store ((float) (threadCpuMs() - c0), std::memory_order_relaxed);

    size_t cpu = 0;
    if (pthread_cpu_number_np (&cpu) == 0)
        lastCpu.store ((int) cpu, std::memory_order_relaxed);
    const auto load = (float) (ms / (1000.0 * numSamples / sampleRate));
    cpuLoad.store (load, std::memory_order_relaxed);
    peakLoad.push (load);
}

void Rack::processAsyncBlock() noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    const int b = pluginBlockSize;
    int s1, n1, s2, n2;

    inFifo.prepareToRead (b, s1, n1, s2, n2);
    for (int ch = 0; ch < 2; ++ch)
    {
        workBuffer.copyFrom (ch, 0, inRing, ch, s1, n1);
        if (n2 > 0) workBuffer.copyFrom (ch, n1, inRing, ch, s2, n2);
    }
    inFifo.finishedRead (n1 + n2);

    {
        const juce::SpinLock::ScopedLockType sl (processLock);
        runChain (workBuffer, b);
    }

    lastThread.store (-1, std::memory_order_relaxed);

    if (outFifo.getFreeSpace() < b)
    {
        xruns.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    outFifo.prepareToWrite (b, s1, n1, s2, n2);
    for (int ch = 0; ch < 2; ++ch)
    {
        outRing.copyFrom (ch, s1, workBuffer, ch, 0, n1);
        if (n2 > 0) outRing.copyFrom (ch, s2, workBuffer, ch, n1, n2);
    }
    outFifo.finishedWrite (n1 + n2);
}

void Rack::exchangeAsync (int numSamples) noexcept
{
    int s1, n1, s2, n2;

    // device -> async thread
    if (inFifo.getFreeSpace() >= numSamples)
    {
        inFifo.prepareToWrite (numSamples, s1, n1, s2, n2);
        for (int ch = 0; ch < 2; ++ch)
        {
            inRing.copyFrom (ch, s1, buffer, ch, 0, n1);
            if (n2 > 0) inRing.copyFrom (ch, s2, buffer, ch, n1, n2);
        }
        inFifo.finishedWrite (n1 + n2);
    }
    else
    {
        xruns.fetch_add (1, std::memory_order_relaxed);
    }

    if (inFifo.getNumReady() >= pluginBlockSize)
        asyncWorker->wake();

    // async thread -> device
    const int ready = juce::jmin (numSamples, outFifo.getNumReady());

    outFifo.prepareToRead (ready, s1, n1, s2, n2);
    for (int ch = 0; ch < 2; ++ch)
    {
        buffer.copyFrom (ch, 0, outRing, ch, s1, n1);
        if (n2 > 0) buffer.copyFrom (ch, n1, outRing, ch, s2, n2);
    }
    outFifo.finishedRead (n1 + n2);

    if (ready < numSamples)
    {
        buffer.clear (0, ready, numSamples - ready);
        buffer.clear (1, ready, numSamples - ready);
        xruns.fetch_add (1, std::memory_order_relaxed);
    }
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

void Rack::process (const float* const* deviceInputs, int numDeviceInputs, int numSamples, int threadIndex) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    const auto t0 = juce::Time::getHighResolutionTicks();
    hasAudio = false;

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

        if (sourceRack != nullptr)
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
            copyIn (1, stereoIn ? inCh + 1 : inCh);
        }

        inMeter[0].push (buffer.getMagnitude (0, 0, numSamples));
        inMeter[1].push (buffer.getMagnitude (1, 0, numSamples));
    };

    auto applyGainAndMeter = [&]
    {
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

        applyCompensation (numSamples);

        outMeter[0].push (buffer.getMagnitude (0, 0, numSamples));
        outMeter[1].push (buffer.getMagnitude (1, 0, numSamples));
        hasAudio = true;
    };

    if (asyncActive.load (std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType al (asyncLock);

        // re-check: a mode change may have started while we were taking the lock
        if (! al.isLocked() || ! asyncActive.load (std::memory_order_acquire) || numSamples > buffer.getNumSamples())
            return;

        readInput();
        exchangeAsync (numSamples);
        applyGainAndMeter();
    }
    else
    {
        const juce::SpinLock::ScopedTryLockType sl (processLock);

        if (! sl.isLocked() || numSamples > buffer.getNumSamples())
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
