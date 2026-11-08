#include "AudioEngine.h"
#include "HopAligner.h"
#include <map>

namespace stagerack
{

static const juce::Colour rackPalette[] = {
    juce::Colour (0xff31c4f3), juce::Colour (0xff7ed957), juce::Colour (0xffffb341), juce::Colour (0xffff5f6d),
    juce::Colour (0xffb98cff), juce::Colour (0xff45e0c0), juce::Colour (0xfff26fd1), juce::Colour (0xffe8e35a)
};

AudioEngine::AudioEngine()
{
    for (int i = 0; i < maxDeviceChannels; ++i)
        inputMap[(size_t) i] = outputMap[(size_t) i] = i;
}

void AudioEngine::setChannelMaps (const juce::BigInteger& activeIns, int numIns, const juce::BigInteger& activeOuts, int numOuts)
{
    auto build = [] (std::array<int, maxDeviceChannels>& map, const juce::BigInteger& active, int num)
    {
        int packed = 0;
        for (int i = 0; i < maxDeviceChannels; ++i)
            map[(size_t) i] = i < num && active[i] ? packed++ : -1;
        return juce::jmin (num, maxDeviceChannels);
    };

    numDeviceInputs = build (inputMap, activeIns, numIns);
    numDeviceOutputs = build (outputMap, activeOuts, numOuts);
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
    clearRacks();
}

void AudioEngine::initialiseDevice (const juce::XmlElement* savedState)
{
    const auto error = deviceManager.initialise (64, 64, savedState, true);

    if (error.isNotEmpty())
        DBG ("Audio device error: " << error);

    deviceManager.addAudioCallback (this);
    deviceManager.addMidiInputDeviceCallback ({}, this);
}

//==============================================================================
int AudioEngine::nextRackId() const
{
    int maxId = 0;
    for (auto& r : racks)
        maxId = juce::jmax (maxId, r->getId());
    return maxId + 1;
}

Rack& AudioEngine::addRack()
{
    return addRackWithId (nextRackId());
}

Rack& AudioEngine::addRackWithId (int id)
{
    auto rack = std::make_unique<Rack> (id);
    const auto index = (int) racks.size();

    rack->name = "Rack " + juce::String (index + 1);
    rack->colour = rackPalette[(size_t) index % std::size (rackPalette)];

    rack->inputChannel = 0;
    rack->outputChannel = 0;
    rack->prepare (sampleRate, blockSize);

    auto& ref = *rack;
    {
        const juce::SpinLock::ScopedLockType sl (racksLock);
        racks.push_back (std::move (rack));
        rebuildGraphLocked();
    }
    return ref;
}

int AudioEngine::indexOf (const Rack& rack) const
{
    for (size_t i = 0; i < racks.size(); ++i)
        if (racks[i].get() == &rack)
            return (int) i;
    return -1;
}

void AudioEngine::removeRack (Rack& rack)
{
    std::unique_ptr<Rack> doomed;
    {
        const juce::SpinLock::ScopedLockType sl (racksLock);

        for (auto it = racks.begin(); it != racks.end(); ++it)
        {
            if (it->get() == &rack)
            {
                doomed = std::move (*it);
                racks.erase (it);
                break;
            }
        }

        rebuildGraphLocked();
    }
    // `doomed` (and its plugins) is destroyed here, on the message thread.
}

void AudioEngine::moveRack (int from, int to)
{
    const juce::SpinLock::ScopedLockType sl (racksLock);

    if (! juce::isPositiveAndBelow (from, (int) racks.size()) || ! juce::isPositiveAndBelow (to, (int) racks.size()))
        return;

    auto item = std::move (racks[(size_t) from]);
    racks.erase (racks.begin() + from);
    racks.insert (racks.begin() + to, std::move (item));
    rebuildGraphLocked();
}

void AudioEngine::clearRacks()
{
    std::vector<std::unique_ptr<Rack>> doomed;
    {
        const juce::SpinLock::ScopedLockType sl (racksLock);
        std::swap (doomed, racks);
        stages.clear();
    }
}

Rack* AudioEngine::findRack (int id) const
{
    for (auto& r : racks)
        if (r->getId() == id)
            return r.get();
    return nullptr;
}

int AudioEngine::choosePhase (int hopCalls, const Rack::Slot* exclude, const Rack* forRack) const
{
    // Least loaded phase among the instances with the same hop. Instances that run in
    // series with this one (same rack, or racks chained to it) weigh far more: their hops
    // would add up on one core, while hops of independent racks run on different cores.
    std::vector<int> cost ((size_t) hopCalls, 0);

    for (auto& r : racks)
    {
        const bool serial = forRack != nullptr && runsInSeries (*r, *forRack);

        for (int i = 0; i < Rack::maxSlots; ++i)
            if (auto& s = r->getSlot (i); &s != exclude && s.plugin != nullptr && s.hopCalls == hopCalls
                                           && juce::isPositiveAndBelow (s.hopPhase.load(), hopCalls))
                cost[(size_t) s.hopPhase.load()] += serial ? 1000 : 1;
    }

    return (int) (std::min_element (cost.begin(), cost.end()) - cost.begin());
}

bool AudioEngine::runsInSeries (const Rack& a, const Rack& b) const
{
    // same rack, or one feeds the other (directly or through other racks)
    auto feeds = [this] (const Rack& from, const Rack& to)
    {
        const Rack* r = &to;

        for (int guard = 0; r != nullptr && guard < 256; ++guard)
        {
            if (r == &from)
                return true;

            r = r->inputRackId >= 0 ? findRack (r->inputRackId) : nullptr;
        }

        return false;
    };

    return feeds (a, b) || feeds (b, a);
}

bool AudioEngine::wouldCreateCycle (const Rack& rack, int sourceRackId) const
{
    int guard = 0;

    for (auto* r = findRack (sourceRackId); r != nullptr && guard++ < 256; r = r->inputRackId >= 0 ? findRack (r->inputRackId) : nullptr)
        if (r == &rack)
            return true;

    return guard >= 256;
}

bool AudioEngine::setRackInputSource (Rack& rack, int sourceRackId)
{
    if (sourceRackId >= 0 && (sourceRackId == rack.getId() || findRack (sourceRackId) == nullptr || wouldCreateCycle (rack, sourceRackId)))
        return false;

    rack.inputRackId = sourceRackId;
    rebuildGraph();
    updateLatencyCompensation();
    return true;
}

void AudioEngine::rebuildGraph()
{
    const juce::SpinLock::ScopedLockType sl (racksLock);
    rebuildGraphLocked();
}

void AudioEngine::rebuildGraphLocked()
{
    // stage = 1 + stage of the source (racks without a valid source are stage 0)
    std::vector<std::vector<Rack*>> newStages;

    for (auto& r : racks)
    {
        r->sourceRack = r->inputRackId >= 0 ? findRack (r->inputRackId) : nullptr;
        r->stage = -1;
    }

    std::function<int (Rack*, int)> stageOf = [&] (Rack* r, int depth) -> int
    {
        if (r->stage >= 0)
            return r->stage;

        if (r->sourceRack == nullptr || depth > 256)
            return r->stage = 0;

        return r->stage = stageOf (r->sourceRack, depth + 1) + 1;
    };

    for (auto& r : racks)
    {
        const auto st = stageOf (r.get(), 0);

        if ((int) newStages.size() <= st)
            newStages.resize ((size_t) st + 1);

        newStages[(size_t) st].push_back (r.get());
    }

    stages = std::move (newStages);
}

void AudioEngine::updateLatencyCompensation()
{
    // latency at each rack's output, before compensation: upstream + own plugins
    std::map<const Rack*, int> base;
    std::function<int (const Rack*, int)> baseOf = [&] (const Rack* r, int depth) -> int
    {
        if (auto it = base.find (r); it != base.end())
            return it->second;

        int upstream = 0;

        if (auto* src = r->inputRackId >= 0 ? findRack (r->inputRackId) : nullptr; src != nullptr && depth < 256)
            upstream = baseOf (src, depth + 1) + src->compensationSamples.load();

        return base[r] = upstream + r->getLatencySamples();
    };

    std::map<int, int> groupMax;

    for (auto& r : racks)
        if (const auto g = r->latencyGroup.load(); g > 0)
            groupMax[g] = juce::jmax (groupMax[g], baseOf (r.get(), 0));

    for (auto& r : racks)
    {
        const auto g = r->latencyGroup.load();
        const auto b = baseOf (r.get(), 0);
        const auto comp = g > 0 ? juce::jlimit (0, Rack::maxCompensation - 1, groupMax[g] - b) : 0;

        r->compensationSamples = comp;
        r->totalLatencySamples = b + comp;
    }
}

void AudioEngine::alignAllRacks()
{
    // Called while the callback is stopped: plugins can be driven directly.
    for (auto& r : racks)
    {
        if (r->isAsync() || ! hopAlignEnabled)
            continue;

        for (int i = 0; i < Rack::maxSlots; ++i)
        {
            auto& slot = r->getSlot (i);

            if (slot.plugin == nullptr)
                continue;

            const auto res = HopAligner::detectAndAlign (*slot.plugin, blockSize,
                                                         [&] (int hop) { return choosePhase (hop, &slot, r.get()); },
                                                         [] { return (juce::uint64) 0; });
            slot.hopCalls = res.hopCalls;
            slot.hopPhase = res.phase;
        }
    }
}

void AudioEngine::alignNewPlugin (juce::AudioPluginInstance& p, Rack& rack, int& hopCalls, int& hopPhase,
                                  const Rack::Slot* replacing)
{
    hopCalls = 0;
    hopPhase = -1;

    if (rack.isAsync() || ! hopAlignEnabled)
        return;

    // +2: the plugin goes live a callback or two after we finish here
    const auto res = HopAligner::detectAndAlign (p, rack.getBlockSize(),
                                                 [this, &rack, replacing] (int hop) { return choosePhase (hop, replacing, &rack); },
                                                 [this] { return callbackCount.load() + 2; });
    hopCalls = res.hopCalls;
    hopPhase = res.phase;
}

void AudioEngine::realignRack (Rack& rack)
{
    if (rack.isAsync() || ! hopAlignEnabled)
        return;

    rack.withChainLocked ([&]
    {
        for (int i = 0; i < Rack::maxSlots; ++i)
        {
            auto& slot = rack.getSlot (i);

            if (slot.plugin == nullptr)
                continue;

            const auto res = HopAligner::detectAndAlign (*slot.plugin, rack.getBlockSize(),
                                                         [&] (int hop) { return choosePhase (hop, &slot, &rack); },
                                                         [this] { return callbackCount.load() + 1; });
            slot.hopCalls = res.hopCalls;
            slot.hopPhase = res.phase;
        }
    });
}

void AudioEngine::setWakeAssistMode (int mode)
{
    wakeAssistMode = mode;
    wakeAssist.setAlwaysOn (mode == 1);
}

void AudioEngine::armWakeAssist (juce::uint64 index) noexcept
{
    if (wakeAssistMode.load (std::memory_order_relaxed) != 0)
        return;

    // Soonest scheduled hop (0 = in this callback).
    int soonest = std::numeric_limits<int>::max();

    for (auto& r : racks)
    {
        for (int i = 0; i < Rack::maxSlots; ++i)
        {
            auto& s = r->getSlot (i);
            const auto hop = s.hopCalls.load (std::memory_order_relaxed);
            const auto phase = s.hopPhase.load (std::memory_order_relaxed);

            if (hop > 1 && phase >= 0)
                soonest = juce::jmin (soonest, (phase - (int) (index % (juce::uint64) hop) + hop) % hop);
        }
    }

    if (soonest == std::numeric_limits<int>::max())
        return;

    if (soonest == 0)
        lastHopCallback = index;

    const auto periodMs = 1000.0 * blockSize / juce::jmax (1.0, sampleRate);
    const auto leadCallbacks = juce::jmax (1, (int) std::ceil (wakeLeadMs.load (std::memory_order_relaxed) / periodMs));

    // Only when the cores would have gone idle before that hop: with dense hops (many
    // instances) they never sleep and the assist would just waste CPU.
    const auto gapMs = (double) (index + (juce::uint64) soonest - lastHopCallback) * periodMs;

    if (soonest >= 1 && soonest <= leadCallbacks && gapMs > 0.8)
    {
        const auto period = juce::Time::secondsToHighResolutionTicks (blockSize / juce::jmax (1.0, sampleRate));
        wakeAssist.armUntil (juce::Time::getHighResolutionTicks() + period * (soonest + 1));
    }
}

void AudioEngine::setWorkerThreads (int numWorkers)
{
    workerSetting = numWorkers;

    // Re-adding the callback re-runs audioDeviceAboutToStart, which restarts the pool.
    deviceManager.removeAudioCallback (this);
    deviceManager.addAudioCallback (this);
}

//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    setChannelMaps (device->getActiveInputChannels(), device->getInputChannelNames().size(),
                    device->getActiveOutputChannels(), device->getOutputChannelNames().size());
    prepare (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(), device->getWorkgroup());
}

void AudioEngine::prepare (double newSampleRate, int newBlockSize, const juce::AudioWorkgroup& workgroup)
{
    sampleRate = newSampleRate;
    blockSize = newBlockSize;

    {
        const juce::SpinLock::ScopedLockType sl (racksLock);
        for (auto& r : racks)
            r->prepare (sampleRate, blockSize);
    }

    workgroupActive = (bool) workgroup;

    int workers = workerSetting < 0 ? WorkerPool::getRecommendedWorkerCount() : workerSetting;

    if (workgroup)
        if (const auto maxParallel = (int) workgroup.getMaxParallelThreadCount(); maxParallel > 1)
            workers = juce::jmin (workers, maxParallel - 1);

    pool.start (workers, blockSize, sampleRate);
    pool.setWorkgroup (workgroup);

    callbackCount = 0;
    alignAllRacks();

    masterGain.reset (sampleRate, 0.02);
    masterGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (masterGainDb.load()));
}

void AudioEngine::audioDeviceStopped()
{
    pool.stop();

    const juce::SpinLock::ScopedLockType sl (racksLock);
    for (auto& r : racks)
        r->release();
}

void AudioEngine::processRackTask (void* ctx, int index, int threadIndex)
{
    auto& self = *static_cast<AudioEngine*> (ctx);
    (*self.currentStage)[(size_t) index]->process (self.cbInputs, self.cbNumInputs, self.cbNumSamples, threadIndex);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                    float* const* outputs, int numOutputs,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto t0 = juce::Time::getHighResolutionTicks();

    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputs[ch], numSamples);

    const auto callbackIndex = callbackCount.fetch_add (1, std::memory_order_relaxed);

    const juce::SpinLock::ScopedTryLockType sl (racksLock);

    if (! sl.isLocked())
        return;

    armWakeAssist (callbackIndex);

    // device channel numbering (what the I/O menus show) -> packed callback buffers
    for (int ch = 0; ch < numDeviceInputs; ++ch)
    {
        const auto idx = inputMap[(size_t) ch];
        deviceIns[(size_t) ch] = juce::isPositiveAndBelow (idx, numInputs) ? inputs[idx] : nullptr;
    }

    for (int ch = 0; ch < numDeviceOutputs; ++ch)
    {
        const auto idx = outputMap[(size_t) ch];
        deviceOuts[(size_t) ch] = juce::isPositiveAndBelow (idx, numOutputs) ? outputs[idx] : nullptr;
    }

    cbInputs = deviceIns.data();
    cbNumInputs = numDeviceInputs;
    cbNumSamples = numSamples;

    for (const auto& stageRacks : stages)
    {
        currentStage = &stageRacks;
        pool.run ((int) stageRacks.size(), &AudioEngine::processRackTask, this, parallelMode.load (std::memory_order_relaxed));
    }

    // Adaptive fan-out: waking worker threads costs a few microseconds, so with a light
    // load it is cheaper to stay on the IO thread. The decision follows the *peak* load of
    // the last seconds (live audio is about the worst block, e.g. FFT plugins), with hysteresis.
    if (forceParallel)
    {
        parallelMode.store (true, std::memory_order_relaxed);
    }
    else
    {
        double workMs = 0.0, heaviestMs = 0.0;

        for (auto& r : racks)
        {
            const auto ms = (double) r->callbackMs.load (std::memory_order_relaxed);
            workMs += ms;
            heaviestMs = juce::jmax (heaviestMs, ms);
        }

        const auto budgetMs = 1000.0 * numSamples / juce::jmax (1.0, sampleRate);
        const auto decay = std::pow (0.5, numSamples / (2.0 * juce::jmax (1.0, sampleRate))); // ~2 s half-life
        peakParallelGainMs = juce::jmax (workMs - heaviestMs, peakParallelGainMs * decay);

        if (parallelMode.load (std::memory_order_relaxed))
        {
            if (peakParallelGainMs < budgetMs * 0.04)
                parallelMode.store (false, std::memory_order_relaxed);
        }
        else if (peakParallelGainMs > budgetMs * 0.08)
        {
            parallelMode.store (true, std::memory_order_relaxed);
        }
    }

    bool anySolo = false;
    for (auto& r : racks)
        anySolo = anySolo || r->solo.load (std::memory_order_relaxed);

    for (auto& r : racks)
    {
        if (! r->producedAudio() || (anySolo && ! r->solo.load (std::memory_order_relaxed)))
            continue;

        const int outCh = r->outputChannel.load (std::memory_order_relaxed);
        const auto& buf = r->getOutput();

        if (outCh < 0)
            continue;   // "OUT: none" (e.g. a rack that only feeds other racks)

        if (! r->stereoOutput.load (std::memory_order_relaxed))
        {
            if (juce::isPositiveAndBelow (outCh, numDeviceOutputs) && deviceOuts[(size_t) outCh] != nullptr)
            {
                juce::FloatVectorOperations::addWithMultiply (deviceOuts[(size_t) outCh], buf.getReadPointer (0), 0.5f, numSamples);
                juce::FloatVectorOperations::addWithMultiply (deviceOuts[(size_t) outCh], buf.getReadPointer (1), 0.5f, numSamples);
            }
            continue;
        }

        for (int i = 0; i < 2; ++i)
            if (juce::isPositiveAndBelow (outCh + i, numDeviceOutputs) && deviceOuts[(size_t) (outCh + i)] != nullptr)
                juce::FloatVectorOperations::add (deviceOuts[(size_t) (outCh + i)], buf.getReadPointer (i), numSamples);
    }

    // master gain + meters on the main pair
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (masterGainDb.load (std::memory_order_relaxed), -100.0f));

    if (masterGain.isSmoothing() || masterGain.getTargetValue() != 1.0f)
    {
        const auto start = masterGain.getCurrentValue();
        masterGain.skip (numSamples);
        const auto end = masterGain.getCurrentValue();

        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
            {
                juce::AudioBuffer<float> view (outputs + ch, 1, numSamples);
                view.applyGainRamp (0, 0, numSamples, start, end);
            }
    }

    const auto elapsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    const auto budget = numSamples / juce::jmax (1.0, sampleRate);
    const auto load = elapsed / budget;
    const auto prev = callbackLoad.load (std::memory_order_relaxed);
    callbackLoad.store (load > prev ? load : prev * 0.9 + load * 0.1, std::memory_order_relaxed);
}

//==============================================================================
void AudioEngine::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    if (m.isProgramChange())
    {
        const auto program = m.getProgramChangeNumber();
        juce::MessageManager::callAsync ([this, program] { if (onProgramChange) onProgramChange (program); });
    }
    else if (m.isController() && m.getControllerValue() > 63)
    {
        const auto cc = m.getControllerNumber();
        const int delta = cc == nextSceneCC.load() ? 1 : (cc == prevSceneCC.load() ? -1 : 0);

        if (delta != 0)
            juce::MessageManager::callAsync ([this, delta] { if (onSceneStep) onSceneStep (delta); });
    }
}

} // namespace stagerack
