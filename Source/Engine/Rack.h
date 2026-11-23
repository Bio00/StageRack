#pragma once
#include "../Plugins/AuWorkgroup.h"

#include <JuceHeader.h>
#include <array>
#include <atomic>

namespace stagerack
{

class TempoEngine;

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
    process them in parallel on separate cores. The chain always runs inside the
    device callback, at the device block size (zero added latency).
*/
class Rack
{
public:
    static constexpr int maxSlots = 8;
    static constexpr int maxPluginChannels = 32;
    /** View of a freshly inserted plugin (setting pluginView; message thread): 1 = mini view. */
    static inline int defaultViewMode = 1;

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
        int viewMode = 1;                  // 0 = parameters, 1 = mini picture of the editor, 2 = plugin meters
        juce::StringArray hiddenMeters;    // names of the plugin meters not shown (message thread only)
        std::atomic<int> hopCalls { 0 };   // detected internal hop, in device callbacks (0 = none)
        std::atomic<int> hopPhase { -1 };  // callback phase its hop was moved to (see HopAligner)

        // Hop watch: the audio thread logs the device callbacks in which this plugin had a
        // heavy call (sync racks only); AudioEngine::watchHops() reads them on the message
        // thread to see where the hop really is (it moves when a plugin is switched on later,
        // e.g. EverClean's Active, or un-bypassed: their frame counter stops while idle).
        static constexpr int heavyRing = 32;
        std::array<std::atomic<juce::uint64>, heavyRing> heavyAt {};
        std::atomic<juce::uint32> heavyCount { 0 };
        float lightMs = 1.0e9f;            // audio thread: cost of an ordinary call (min-tracking)
        // Time the audio thread spends blocked inside the plugin's hop call (wall - thread CPU,
        // smoothed), -1 = not measured yet. De-Feedback waits for its helper thread (~0.1-0.25 ms);
        // EverClean computes the whole hop on the calling thread (~0). The core wake assist
        // is only useful for the first kind.
        std::atomic<float> hopWaitMs { -1.0f };
        // Does this instance need CoreWakeAssist? It starts without; an unassisted hop after an
        // idle gap that waits long for the plugin's helper turns it on for good (De-Feedback
        // VST3 waits ~0.34 ms; its AU version and EverClean ~0.02, where the assist burnt a
        // core for nothing). Remembered per plugin type (AudioEngine::watchHops).
        std::atomic<int> wakeNeed { 0 };
        float wakeProbeMs = 0.0f;   // audio thread: worst unassisted wait seen
        // message thread only (watchHops)
        const void* watchPlugin = nullptr;
        juce::uint64 watchSince = 0;
        int watchHop = 0, watchPhase = -1, watchSeen = 0, watchRespreads = 0;

        // Seamless hand-over (live re-spread): a twin instance, already re-phased offline,
        // runs on the same input next to `plugin`, then a crossfade hands the output over.
        // The fields below are written by the message thread under processLock, then only
        // by the audio thread until handoverDone; afterwards `twin` holds the retired instance.
        std::unique_ptr<juce::AudioPluginInstance> twin;
        auwg::Context au, auTwin;   // AU render context observers of plugin / twin (workgroup)
        int twinHopCalls = 0, twinHopPhase = -1;
        int twinWarmSamples = 0, twinFadeSamples = 0, twinPos = 0;
        juce::uint64 twinFirstCallback = 0;        // callback the twin's alignment assumed it starts in
        std::atomic<int> twinLateBy { 0 };         // diagnostics: how many callbacks later it really started
        std::atomic<bool> handoverDone { false };
    };

    explicit Rack (int uniqueId);
    ~Rack();

    int getId() const noexcept                      { return id; }

    /** Index of the device callback being processed (set by the engine before each callback). */
    juce::uint64 callbackIndex = 0;
    static inline std::atomic<bool> wakeProbeNow { false };
    static constexpr float wakeNeedMs = 0.08f;   // unassisted wait above this = the assist helps   // set by the engine: this callback's hops are unassisted after an idle gap

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
        (and gives the twin back through `twin`) if the slot is empty or busy. */
    bool beginHandover (int slot, std::unique_ptr<juce::AudioPluginInstance>& twin, int hopCalls, int hopPhase, juce::uint64 firstCallback,
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

    /** Plugin latency (this rack only, no upstream/compensation). */
    int getLatencySamples() const noexcept;

    void prepare (double sampleRate, int deviceBlockSize);
    void release();


    double getSampleRate() const noexcept           { return sampleRate; }
    /** Block size the plugins are prepared with (= device block). */
    int getBlockSize() const noexcept               { return pluginBlockSize; }

    //==============================================================================
    // Realtime parameters
    std::atomic<float> gainDb { 0.0f };
    std::atomic<float> inputGainDb { 0.0f };   // trim before the first plugin
    static constexpr float minInputGainDb = -24.0f, maxInputGainDb = 24.0f;
    std::atomic<bool> muted { false }, bypassed { false }, solo { false };
    std::atomic<int> inputChannel { 0 };    // first device input channel, -1 = none
    std::atomic<bool> stereoInput { true };
    std::atomic<int> outputChannel { 0 };   // first device output channel, -1 = none
    std::atomic<bool> stereoOutput { true }; // false = (L+R)/2 on a single output channel

    float uiSize = 1.0f;                     // rack height (horizontal) / width (vertical) factor
    // Dividers inside the rack (pixels, -1 = automatic), per layout: room for the sends (between
    // plugins and fader) and for the input section (trim + meter, above the plugins).
    float uiSendsV = -1.0f, uiSendsH = -1.0f, uiInputV = -1.0f, uiOutputV = -1.0f;
    int uiHideSections = -1; // this rack's own sections (bits as Session::hiddenBits: 1 I/O, 2 input, 4 inserts, 8 sends, 16 info); -1 = as the session's view
    int uiSendsShown = -1;   // sends this rack shows: bit k = Bus k+1; -1 = as the session's view (Session::RackView)

    // Folder (Ableton group track, message thread only): a folder is a bus rack that its member
    // racks are routed into; members follow it in the rack list (Session::normalizeFolders) and
    // are hidden while it is closed. folderId = the folder this rack is in (-1 = top level).
    bool isFolder = false, folderOpen = true;
    int folderId = -1;

    // Edit group (Pro Tools style, message thread only): racks of the same group (1..8 = A..H,
    // 0 = none) follow each other's edits while the group is active (Session::isEditGroupActive).
    int editGroup = 0;
    static constexpr int maxEditGroups = 8;

    // Tempo: the plugins of this rack get this tempo (0 = master, 1..4) and its beat position
    // through their AudioPlayHead (synced delays, LFOs...). Zero cost when no plugin asks.
    std::atomic<int> tempoIndex { 0 };
    /** Audio for the tempo detectors (TempoEngine::AudioPoint 1 pre plugins, 2 post plugins,
        3 post fader): bit 1 << point, set by the engine before the racks run. Mono (L+R)/2. */
    std::atomic<int> tempoTapMask { 0 };
    const float* getTempoTap (int point) const noexcept
    {
        return point >= 1 && point <= 3 && tempoTapValid[(size_t) point] ? tempoTapBuffer.getReadPointer (point - 1) : nullptr;
    }
    const TempoEngine* tempo = nullptr;     // set by AudioEngine
    juce::AudioPlayHead* getPlayHead() noexcept { return &playHead; }

    // Chaining: input from another rack's output instead of the device (-1 = device).
    // Set through AudioEngine::setRackInputSource (it rebuilds the processing stages).
    std::atomic<int> inputRackId { -1 };

    // Buses and sends (Ableton style; only while AudioEngine::routingEnabled). A bus is a rack
    // whose input is the sum of what the other racks route or send to it (FX return, sub-group,
    // master bus...). It runs in a later stage of the same callback: zero latency.
    static constexpr int maxBuses = 8;            // A..H
    static constexpr float sendOffDb = -100.0f;   // a send at or below this is off
    std::atomic<int> busIndex { 0 };              // 1..maxBuses: this rack is Bus A..H (message thread: AudioEngine::setRackBus)
    std::atomic<int> outputBus { 0 };             // 0 = device outputs, 1..maxBuses = into that bus (AudioEngine::setRackOutputBus)

    // More destinations besides the main one (outputChannel/stereoOutput or outputBus), Pro Tools
    // style "multiple outputs". Same codes as the OUT menu: outStereo + ch = device pair from ch,
    // outMono + ch = device channel ch, outBus + k = Bus k (only while routing is on); 0 = unused.
    // Written on the message thread (AudioEngine::setRackExtraOutputs), read by the audio thread.
    static constexpr int maxExtraOutputs = 7, outNone = 1, outStereo = 1000, outMono = 2000, outBus = 4000;
    std::array<std::atomic<int>, maxExtraOutputs> extraOutputs {};
    std::vector<int> getExtraOutputs() const
    {
        std::vector<int> v;
        for (auto& o : extraOutputs)
            if (const auto c = o.load (std::memory_order_relaxed); c > 0)
                v.push_back (c);
        return v;
    }
    /** The main destination as a code (outBus + k only while `routing`). */
    int mainOutputCode (bool routing) const noexcept
    {
        if (const auto b = outputBus.load(); routing && b > 0) return outBus + b;
        const auto ch = outputChannel.load();
        return ch < 0 ? outNone : (stereoOutput.load() ? outStereo : outMono) + ch;
    }
    struct Send
    {
        std::atomic<float> db { sendOffDb };
        std::atomic<bool> pre { false };          // pre-fader (after the plugins, before gain/automix; follows mute)
        bool isOn() const noexcept                { return db.load (std::memory_order_relaxed) > sendOffDb; }
    };
    std::array<Send, maxBuses> sends;             // index = bus letter - 1 (set levels through AudioEngine::setSend)

    // Routing graph (written by AudioEngine under its racks lock, read by the audio thread;
    // the message thread may read it too, it is its only writer).
    struct Feed
    {
        Rack* src = nullptr;
        int kind = 0;           // 0 = chain (input from rack), 1 = routed output, 2 = send
        int send = -1;          // kind 2: send index (bus letter - 1)
        float gain = 0.0f;      // audio thread (this bus): gain applied in the last block
    };
    std::vector<Feed> feeds;                      // everything that runs before this rack and feeds it
    Rack* outBusRack = nullptr;                   // routed into this bus (else device outputs)
    bool outToBus = false;                        // routed to a bus: never summed into the device (bus missing = silent)
    bool feedsABus = false;                       // some output (main or extra) reaches a bus
    bool isActiveBus = false;                     // input = sum of the feeds
    std::atomic<juce::uint32> sendAllowed { 0 };  // bit k: a send to bus k+1 is possible (exists, no loop)
    bool feedIsOn (const Feed& f) const noexcept  { return f.kind != 2 || f.src->sends[(size_t) f.send].isOn(); }

    // Solo, per callback (engine): heard = soloed or fed by a soloed rack; feeds = feeds a soloed rack.
    bool anySolo = false, soloHeard = false, soloFeeds = false;
    bool passesSolo() const noexcept              { return ! anySolo || soloHeard || soloFeeds; }

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
    std::atomic<juce::uint32> nonFiniteBlocks { 0 };   // blocks silenced because they held inf / NaN
    std::atomic<float> cpuMs { 0.0f };       // chain time per processed block
    std::atomic<float> cpuLoad { 0.0f };     // chain time / block duration (0..1+)
    PeakMeter peakLoad;                      // max cpuLoad since last read
    std::atomic<float> callbackMs { 0.0f };  // time spent on the device callback
    std::atomic<int> lastThread { 0 };       // worker index the chain last ran on
    std::atomic<int> lastCpu { -1 };         // CPU core the chain last ran on (0-3 = E-cores on M4 Pro)
    std::atomic<float> cpuTimeMs { 0.0f };   // thread CPU time used by the chain (vs cpuMs = wall time)

    //==============================================================================
    // Audio thread
    void process (const float* const* deviceInputs, int numDeviceInputs, int numSamples, int threadIndex) noexcept;
    const juce::AudioBuffer<float>& getOutput() const noexcept { return buffer; }
    /** After the plugins and the latency compensation, before gain/mute (valid if hasPreFader()). */
    const juce::AudioBuffer<float>& getPreFader() const noexcept { return preBuffer; }
    bool hasPreFader() const noexcept               { return preValid; }
    bool producedAudio() const noexcept             { return hasAudio; }

private:
    void runChain (juce::AudioBuffer<float>&, int numSamples) noexcept;   // processLock held
    void processSlot (Slot&, juce::AudioBuffer<float>&, int numSamples) noexcept;

    const int id;
    std::array<Slot, maxSlots> slots;
    juce::SpinLock processLock;     // plugin chain structure

    juce::AudioBuffer<float> buffer;       // device-side rack bus
    juce::AudioBuffer<float> preBuffer;    // pre-fader copy, only while a pre-fader send is on
    juce::AudioBuffer<float> tempoTapBuffer;   // tempo detector taps (see tempoTapMask)
    std::array<bool, 4> tempoTapValid {};
    void captureTempoTap (int point, int numSamples) noexcept;
    bool preValid = false;
    void sumFeeds (int numSamples) noexcept;   // bus input
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
        with --automix-probe, see docs/MANUALE.md (Automixer). */
    static inline std::atomic<float> automixAvgMs { 5.0f }, automixHoldMs { 40.0f }, automixReleaseDbPerSec { 100.0f },
                                     automixHighPassHz { 200.0f }, automixLowPassHz { 4000.0f };
private:
    struct TempoPlayHead final : juce::AudioPlayHead
    {
        explicit TempoPlayHead (const Rack& r) : rack (r) {}
        juce::Optional<PositionInfo> getPosition() const override;
        const Rack& rack;
    };
    TempoPlayHead playHead { *this };

    juce::AudioBuffer<float> twinBuffer;   // hand-over: the twin's copy of the slot input
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> discarded;   // message thread, see takeDiscarded()
    void dropTwin (Slot&);                 // processLock held
    juce::MidiBuffer midi;
    juce::LinearSmoothedValue<float> gain { 1.0f }, inGain { 1.0f };
    double sampleRate = 48000.0;
    int deviceBlockSize = 512, pluginBlockSize = 512;
    bool hasAudio = false;

    JUCE_DECLARE_NON_COPYABLE (Rack)
};

} // namespace stagerack
