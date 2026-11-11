#pragma once

#include <array>

#include "Rack.h"
#include "WorkerPool.h"
#include "CoreWakeAssist.h"

namespace stagerack
{

/**
    Owns the audio device and the racks, and runs the parallel audio callback:

        device inputs ──► [rack 1] ─┐
                      ├─► [rack 2] ─┤   (racks run concurrently on the WorkerPool)
                      └─► [rack N] ─┴─► sum into device outputs
*/
class AudioEngine final : public juce::AudioIODeviceCallback,
                          private juce::MidiInputCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    void initialiseDevice (const juce::XmlElement* savedState);
    juce::AudioDeviceManager& getDeviceManager() noexcept   { return deviceManager; }

    //==============================================================================
    // Racks (message thread)
    Rack& addRack();
    void removeRack (Rack&);
    void moveRack (int from, int to);
    void clearRacks();
    int getNumRacks() const noexcept                         { return (int) racks.size(); }
    Rack& getRack (int index) const                          { return *racks[(size_t) index]; }
    Rack* findRack (int id) const;
    int indexOf (const Rack&) const;

    //==============================================================================
    // Chaining and latency groups (message thread)
    /** Feeds `rack` from another rack's output (-1 = the device input). Refused (returns
        false) if it would create a loop. Zero latency: the source runs in an earlier stage
        of the same callback. */
    bool setRackInputSource (Rack&, int sourceRackId);
    bool wouldCreateCycle (const Rack&, int sourceRackId) const;

    /** Recomputes the compensation delays of latency groups (cheap; call periodically, as
        plugins may change their latency). */
    void updateLatencyCompensation();
    int nextRackId() const;
    Rack& addRackWithId (int id);

    //==============================================================================
    /** 0 = audio thread only; -1 = automatic (P-cores - 1). Restarts the device. */
    void setWorkerThreads (int numWorkers);
    int getWorkerThreadsSetting() const noexcept             { return workerSetting; }
    void presetWorkerThreads (int numWorkers) noexcept       { workerSetting = numWorkers; } // before initialiseDevice()
    const WorkerPool& getPool() const noexcept               { return pool; }
    WorkerPool& getPool() noexcept                           { return pool; }

    //==============================================================================
    /** Zero-latency hop spreading (see HopAligner). */
    std::atomic<bool> hopAlignEnabled { true };

    /** Aligns a new, prepared plugin before it is put in `rack` (message thread). */
    void alignNewPlugin (juce::AudioPluginInstance&, Rack&, int& hopCalls, int& hopPhase,
                        const Rack::Slot* replacing = nullptr);   // replacing: its phase does not count as used

    /** Re-aligns every plugin of a running sync rack (after its plugins were re-prepared). */
    void realignRack (Rack&);

    juce::uint64 getCallbackCount() const noexcept           { return callbackCount.load (std::memory_order_relaxed); }

    /** Core wake assist (see CoreWakeAssist): 0 = auto (around plugin hops), 1 = always, 2 = off. */
    void setWakeAssistMode (int mode);
    int getWakeAssistMode() const noexcept                   { return wakeAssistMode; }

    std::atomic<float> masterGainDb { 0.0f };

    /** Automixer (Dugan-style gain sharing) on/off for all groups. Off = every rack at unity. */
    std::atomic<bool> automixEnabled { true };

    /** What happens when several people talk at once: 0 = share the gain (Dugan: the room
        sounds like one open mic, two equal talkers -3 dB each), 1 = soft (up to 1.5 mics' worth
        of gain while several talk: -1.2 dB each), 2 = open (one mic's worth per talker: 0 dB
        each). With one talker or nobody talking all three are exactly Dugan's law. */
    std::atomic<int> automixOverlap { 0 };

    /** The overlap modes: scales a group's Dugan gains so their total power reaches
        min (number of talkers, maxOpen), each gain capped at unity. talk[i] = 0..1. */
    static void automixOverlapGains (float* gains, const float* talk, int n, float maxOpen) noexcept;

    /** The gain-sharing law (public for the self test): gains[i] = sqrt (P_i / sum_j P_j), P = weighted
        power: sum of gains^2 = 1, the group's noise stays that of one mic.
        Returns false (gains untouched: "last mic hold") when the whole group is silent. */
    static bool automixGains (const float* weightedPowers, float* gains, int n) noexcept;

    double getSampleRate() const noexcept                    { return sampleRate; }
    int getBlockSize() const noexcept                        { return blockSize; }
    /** Device channel -> packed callback buffer maps (set when the device starts; public for tests). */
    void setChannelMaps (const juce::BigInteger& activeIns, int numIns, const juce::BigInteger& activeOuts, int numOuts);

    double getCallbackLoad() const noexcept                  { return callbackLoad.load (std::memory_order_relaxed); }
    bool hasWorkgroup() const noexcept                       { return workgroupActive; }
    bool isRunningParallel() const noexcept                  { return parallelMode.load (std::memory_order_relaxed); }
    bool forceParallel = false;                              // disables the adaptive mode (benchmarks)

    /** Called on the message thread on MIDI program change (0-based) or the next/prev CCs. */
    std::function<void (int program)> onProgramChange;
    std::function<void (int delta)> onSceneStep;
    std::atomic<int> nextSceneCC { 80 }, prevSceneCC { 81 };

    //==============================================================================
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    /** Same as audioDeviceAboutToStart() without a device (benchmarks / tests). */
    void prepare (double sampleRate, int blockSize, const juce::AudioWorkgroup& workgroup);

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    static void processRackTask (void* ctx, int index, int threadIndex);
    void computeAutomixGains (bool anySolo) noexcept;

    juce::AudioDeviceManager deviceManager;
    WorkerPool pool;
    int workerSetting = -1;

    std::vector<std::unique_ptr<Rack>> racks;
    juce::SpinLock racksLock;           // held by the audio callback while it uses `racks`

    // Processing stages: racks of a stage only depend on earlier stages, so each stage
    // runs in parallel. Rebuilt on the message thread, swapped in under racksLock.
    std::vector<std::vector<Rack*>> stages;
    const std::vector<Rack*>* currentStage = nullptr;
    void rebuildGraph();
    void rebuildGraphLocked();

    // callback scratch (valid only during a callback)
    const float* const* cbInputs = nullptr;
    int cbNumInputs = 0, cbNumSamples = 0;

    // The device callback gets only the ENABLED channels, packed together; racks use device
    // channel numbers. These map device channel -> callback buffer index (-1 = disabled).
    // Identity until a device starts (offline tests call the callback directly).
    static constexpr int maxDeviceChannels = 256;
    std::array<int, maxDeviceChannels> inputMap {}, outputMap {};
    int numDeviceInputs = maxDeviceChannels, numDeviceOutputs = maxDeviceChannels;
    std::array<const float*, maxDeviceChannels> deviceIns {};
    std::array<float*, maxDeviceChannels> deviceOuts {};

    double sampleRate = 48000.0;
    int blockSize = 512;
    bool workgroupActive = false;
    int choosePhase (int hopCalls, const Rack::Slot* exclude, const Rack* forRack) const;
    bool runsInSeries (const Rack&, const Rack&) const;
    void alignAllRacks();

    std::atomic<juce::uint64> callbackCount { 0 };
    void armWakeAssist (juce::uint64 callbackIndex) noexcept;

    CoreWakeAssist wakeAssist { 4 };
    juce::uint64 lastHopCallback = 0;   // audio thread only
    std::atomic<int> wakeAssistMode { 0 };

public:
    /** How long before a hop the cores are woken (the spinners themselves need time to get a
        running core). Measured on M4 Pro: ~1.5-2 ms is needed at 16 samples. */
    std::atomic<double> wakeLeadMs { 2.0 };

    void setWakeAssistSpinners (int n) noexcept              { wakeAssist.setActiveSpinners (n); }

private:
    std::atomic<bool> parallelMode { false };   // adaptive: only fan out when the load is worth it
    double peakParallelGainMs = 0.0;
    juce::LinearSmoothedValue<float> masterGain { 1.0f };
    std::atomic<double> callbackLoad { 0.0 };

    JUCE_DECLARE_NON_COPYABLE (AudioEngine)
};

} // namespace stagerack
