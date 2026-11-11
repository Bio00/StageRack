#pragma once

#include <JuceHeader.h>
#include <atomic>

namespace stagerack
{

/** A peak meter value written by the audio thread and read (and decayed) by the UI. */
struct PeakMeter
{
    std::atomic<float> peak { 0.0f };

    void push (float v) noexcept
    {
        auto prev = peak.load (std::memory_order_relaxed);
        while (v > prev && ! peak.compare_exchange_weak (prev, v, std::memory_order_relaxed)) {}
    }

    float readAndReset() noexcept { return peak.exchange (0.0f, std::memory_order_relaxed); }
};

/**
    A rack: a stereo bus with up to `maxSlots` plugins in series.
    Racks are independent of each other, which is what lets the engine
    process them in parallel on separate cores.

    Two processing modes:
    - sync:  the chain runs inside the device callback, at the device block size.
    - async: the chain runs on its own realtime thread at a larger internal block
             (factor x device block, e.g. 64 while the device runs at 32). Heavy frame-based plugins
             (De-Feedback, denoisers...) then spread their CPU spikes over several
             device callbacks instead of having to finish within one tiny buffer.
             Cost: asyncBlock + safety + 2 x deviceBlock samples of extra latency, on this rack only.
*/
class Rack
{
public:
    static constexpr int maxSlots = 8;
    static constexpr int maxPluginChannels = 32;

    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        std::atomic<bool> bypassed { false };
        std::atomic<float> cpuMs { 0.0f };
        int numChannels = 2;    // channels handed to processBlock
        bool mono = false;      // 1-in/1-out plugin on a stereo rack
        juce::Array<int> shownParams;      // parameter indices shown on the rack (message thread only)
        bool paramsChosen = false;         // false = use the default selection
        float uiSize = 1.0f;               // slot width (horizontal) / height (vertical) weight in the rack
        int viewMode = 0;                  // 0 = parameters, 1 = mini picture of the editor, 2 = plugin meters
        std::atomic<int> hopCalls { 0 };   // detected internal hop, in device callbacks (0 = none)
        std::atomic<int> hopPhase { -1 };  // callback phase its hop was moved to (see HopAligner)

        // Seamless hand-over (live re-spread): a twin instance, already re-phased offline,
        // runs on the same input next to `plugin`, then a crossfade hands the output over.
        // The fields below are written by the message thread under processLock, then only
        // by the audio thread until handoverDone; afterwards `twin` holds the retired instance.
        std::unique_ptr<juce::AudioPluginInstance> twin;
        int twinHopCalls = 0, twinHopPhase = -1;
        int twinWarmSamples = 0, twinFadeSamples = 0, twinPos = 0;
        std::atomic<bool> handoverDone { false };
    };

    explicit Rack (int uniqueId);
    ~Rack();

    int getId() const noexcept                      { return id; }

    //==============================================================================
    // Message thread
    juce::String name;
    juce::Colour colour;

    /** Prepares a plugin for this rack (bus layout, sample rate). Call before setPlugin(). */
    static void configurePlugin (juce::AudioPluginInstance&, double sampleRate, int blockSize);

    /** Swaps a plugin into a slot; returns the previous one so the caller can delete it
        on the message thread (after closing its editor). */
    std::unique_ptr<juce::AudioPluginInstance> setPlugin (int slot, std::unique_ptr<juce::AudioPluginInstance>);
    void moveSlot (int from, int to);

    /** Starts a seamless hand-over of `slot` to `twin` (same plugin, same state, prepared for
        this rack): the twin processes the live input in parallel for `warmSamples`, then the
        output crossfades to it over `fadeSamples`. No gap, no latency change. Returns false
        (and gives the twin back through `twin`) if the slot is empty, async or busy. */
    bool beginHandover (int slot, std::unique_ptr<juce::AudioPluginInstance>& twin, int hopCalls, int hopPhase,
                        int warmSamples, int fadeSamples);
    bool isHandingOver (int slot) const noexcept;
    /** Message thread: once the hand-over has finished, returns the retired instance (to be
        deleted on the message thread), else nullptr. */
    std::unique_ptr<juce::AudioPluginInstance> collectHandover (int slot);
    /** Instances dropped by an interrupted hand-over (slot replaced, re-prepared...): delete them
        on the message thread. */
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> takeDiscarded();

    /** Runs fn with the chain locked (the audio thread skips this rack meanwhile). */
    template <typename Fn>
    void withChainLocked (Fn&& fn)
    {
        const juce::SpinLock::ScopedLockType sl (processLock);
        fn();
    }

    juce::AudioPluginInstance* getPlugin (int slot) const noexcept;
    Slot& getSlot (int slot) noexcept               { return slots[(size_t) slot]; }
    int getNumPlugins() const noexcept;

    /** Plugin latency + the extra buffering of async mode (this rack only, no upstream/compensation). */
    int getLatencySamples() const noexcept;

    void prepare (double sampleRate, int deviceBlockSize);
    void release();

    /** 0 = sync (device callback). 2/4/8 = async thread with an internal block of
        factor x device block, so async is always engaged whatever the device buffer. */
    void setAsyncFactor (int factor);
    int getAsyncFactor() const noexcept             { return asyncFactor; }
    int getDeviceBlockSize() const noexcept         { return deviceBlockSize; }
    bool isAsync() const noexcept                   { return asyncActive.load (std::memory_order_acquire); }
    int getAddedLatencySamples() const noexcept;

    /** Extra async buffering (ms) that absorbs a plugin's occasional CPU spikes
        (global; applied on the next prepare / async change). */
    static inline std::atomic<float> asyncSafetyMs { 3.0f };

    double getSampleRate() const noexcept           { return sampleRate; }
    /** Block size the plugins are prepared with (device block, or the async block). */
    int getBlockSize() const noexcept               { return pluginBlockSize; }

    //==============================================================================
    // Realtime parameters
    std::atomic<float> gainDb { 0.0f };
    std::atomic<bool> muted { false }, bypassed { false }, solo { false };
    std::atomic<int> inputChannel { 0 };    // first device input channel, -1 = none
    std::atomic<bool> stereoInput { true };
    std::atomic<int> outputChannel { 0 };   // first device output channel, -1 = none
    std::atomic<bool> stereoOutput { true }; // false = (L+R)/2 on a single output channel

    float uiSize = 1.0f;                     // rack height (horizontal) / width (vertical) factor

    // Chaining: input from another rack's output instead of the device (-1 = device).
    // Set through AudioEngine::setRackInputSource (it rebuilds the processing stages).
    std::atomic<int> inputRackId { -1 };

    // Latency groups (SuperRack style): racks of the same group (1..8 = A..H, 0 = none)
    // are delayed to the group's highest latency, so they stay time-aligned.
    std::atomic<int> latencyGroup { 0 };
    std::atomic<int> compensationSamples { 0 }; // set by AudioEngine::updateLatencyCompensation
    std::atomic<int> totalLatencySamples { 0 }; // upstream + plugins + compensation (for display)
    static constexpr int maxCompensation = 32768;

    // Automixer (gain sharing, see AudioEngine::computeAutomixGains). The rack measures its own
    // level (after the plugins and its fader, as Dugan Speech does); the engine applies the shared gain where it
    // sums the rack into the device outputs. Zero latency, a few flops per sample.
    static constexpr int maxAutomixGroups = 4;   // A..D
    std::atomic<int> automixGroup { 0 };         // 0 = not automixed, 1..maxAutomixGroups
    std::atomic<float> automixWeightDb { 0.0f }; // bias of this rack in the group (dB)
    std::atomic<float> automixGainDb { 0.0f };   // gain the automixer applies now (display)
    std::atomic<float> automixLevelDb { -100.0f };   // detector level, dBFS RMS (display)
    float automixTalk = 0.0f;                    // audio thread: 0..1, how clearly this mic is above its own noise floor
    float automixLevel = 0.0f;                   // audio thread: detector power, post-fader
    float automixTarget = 1.0f, automixGain = 1.0f;   // audio thread (engine): target / last applied gain

    // Engine graph data: written by AudioEngine under its racks lock, read by the audio thread.
    Rack* sourceRack = nullptr;
    int stage = 0;

    PeakMeter inMeter[2], outMeter[2];
    std::atomic<float> cpuMs { 0.0f };       // chain time per processed block
    std::atomic<float> cpuLoad { 0.0f };     // chain time / block duration (0..1+)
    PeakMeter peakLoad;                      // max cpuLoad since last read
    std::atomic<float> callbackMs { 0.0f };  // time spent on the device callback
    std::atomic<int> lastThread { 0 };       // -1 = own async thread
    std::atomic<int> lastCpu { -1 };         // CPU core the chain last ran on (0-3 = E-cores on M4 Pro)
    std::atomic<float> cpuTimeMs { 0.0f };   // thread CPU time used by the chain (vs cpuMs = wall time)
    std::atomic<int> xruns { 0 };            // async buffer under/overruns

    //==============================================================================
    // Audio thread
    void process (const float* const* deviceInputs, int numDeviceInputs, int numSamples, int threadIndex) noexcept;
    const juce::AudioBuffer<float>& getOutput() const noexcept { return buffer; }
    bool producedAudio() const noexcept             { return hasAudio; }

private:
    class AsyncWorker;

    void runChain (juce::AudioBuffer<float>&, int numSamples) noexcept;   // processLock held
    void processSlot (Slot&, juce::AudioBuffer<float>&, int numSamples) noexcept;
    void processAsyncBlock() noexcept;                                    // async thread
    void exchangeAsync (int numSamples) noexcept;                         // device thread
    void stopAsync();
    void startAsyncIfNeeded();

    const int id;
    std::array<Slot, maxSlots> slots;
    juce::SpinLock processLock;     // plugin chain structure
    juce::SpinLock asyncLock;       // async FIFOs vs. mode changes

    juce::AudioBuffer<float> buffer;       // device-side rack bus
    juce::AudioBuffer<float> delayLine;    // latency-group compensation (2 x maxCompensation)
    int delayWrite = 0;
    void applyCompensation (int numSamples) noexcept;
    void updateAutomixLevel (int numSamples) noexcept;

    // automix detector (audio thread): voice-band filter -> 5 ms power average -> dB envelope
    // (instant rise, hold, then a constant dB/s fall)
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void set (const juce::IIRCoefficients& c) noexcept
        {
            b0 = c.coefficients[0]; b1 = c.coefficients[1]; b2 = c.coefficients[2];
            a1 = c.coefficients[3]; a2 = c.coefficients[4];
        }
        void reset() noexcept { z1 = z2 = 0.0f; }
        float process (float x) noexcept
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    Biquad amHighPass, amLowPass;
    float amAvgCoeff = 0.0f, amAvg = 0.0f, amEnvDb = -200.0f, amReleaseDbPerSample = 0.0f;
    float amFloorDb = -200.0f, amFloorRisePerSample = 0.0f;   // noise floor: follows minima, rises slowly
    int amHoldSamples = 0, amHoldLeft = 0;
    bool amActive = false;

public:
    /** Automix detector tuning (global; applied on the next prepare). Tuned against speech
        with --automix-probe, see README. */
    static inline std::atomic<float> automixAvgMs { 5.0f }, automixHoldMs { 40.0f }, automixReleaseDbPerSec { 100.0f },
                                     automixHighPassHz { 200.0f }, automixLowPassHz { 4000.0f };
private:
    juce::AudioBuffer<float> workBuffer;   // async thread chain buffer
    juce::AudioBuffer<float> twinBuffer;   // hand-over: the twin's copy of the slot input
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> discarded;   // message thread, see takeDiscarded()
    void dropTwin (Slot&);                 // processLock held
    juce::MidiBuffer midi;
    juce::LinearSmoothedValue<float> gain { 1.0f };
    double sampleRate = 48000.0;
    int deviceBlockSize = 512, pluginBlockSize = 512;
    bool hasAudio = false;

    // async
    int asyncFactor = 0;
    std::atomic<bool> asyncActive { false };
    int asyncPrefill = 0;
    juce::AbstractFifo inFifo { 2 }, outFifo { 2 };
    juce::AudioBuffer<float> inRing, outRing;
    std::unique_ptr<AsyncWorker> asyncWorker;

    JUCE_DECLARE_NON_COPYABLE (Rack)
};

} // namespace stagerack
