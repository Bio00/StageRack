#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

namespace stagerack
{

/** Tap tempo from a series of tap times. The period is the least-squares line through the last
    taps (every tap counts, not only the first and the last one), so a single late tap moves the
    tempo little and more taps keep refining it. A tap far off the current period starts a new
    series (the tempo changed), a pause starts over, a bounce (double trigger) is ignored. */
class TapEstimator
{
public:
    struct Settings
    {
        int maxTaps = 8;                 // taps in the fit
        double resetSec = 2.0;           // a longer pause starts a new series
        double changeTolerance = 0.25;   // interval this far off the period (fraction) = new tempo
        double minIntervalSec = 0.12;    // shorter = bounce, ignored
    };

    /** Returns false if the tap was ignored (bounce). */
    bool tap (double timeSec, const Settings&);
    void reset() noexcept                          { count = 0; }
    int getNumTaps() const noexcept                { return count; }
    bool hasPeriod() const noexcept                { return count >= 2; }
    double getPeriod() const noexcept              { return period; }
    /** Where the fitted line puts the last tap (less jitter than the tap itself). */
    double getLastBeatTime() const noexcept        { return lastBeat; }

private:
    static constexpr int capacity = 32;
    std::array<double, capacity> times {};
    int count = 0;
    double period = 0.5, lastBeat = 0.0;
    void fit() noexcept;
};

/** Beat tracking from drum onsets (e.g. a kick mic): the period that explains most of the
    intervals between the onsets of the last seconds (any pair, multiples of the beat count),
    searched within half an octave of the current tempo so it never jumps to double/half time,
    then refined by a least-squares fit on the onsets that sit on the grid. */
class BeatTracker
{
public:
    void addOnset (double timeSec);
    void clear()                                   { onsets.clear(); }
    int getNumOnsets() const noexcept              { return (int) onsets.size(); }

    struct Result { double bpm = 0.0, beatTime = 0.0, confidence = 0.0; };
    /** false = not enough onsets. centreBpm: the search covers centre / sqrt 2 .. centre * sqrt 2. */
    bool estimate (double nowSec, double centreBpm, Result&) const;

    static constexpr double windowSec = 8.0;

private:
    std::vector<double> onsets;
};

/**
    Tempo for the plugins (delays, modulations synced to the host): a MASTER tempo and four more,
    each one either following the master (x ratio) or with its own tempo. Every rack plays with one
    of them (Rack::tempoIndex): its plugins see that tempo, and a beat position in phase with the
    taps, through the rack's AudioPlayHead. Zero latency, nothing changes in the audio itself.

    Tap sources, any number per tempo: the TAP buttons (on mouse down), a computer key, a MIDI
    note or CC (footswitch, pad), MIDI clock in, and an audio input: every transient is a tap
    (a click track, a pad) or the beat is tracked from a drum (kick mic).

    Threads: the message thread and the MIDI thread use the API under a mutex; the audio thread
    only reads each tempo's grid (lock-free) and runs the audio onset detectors.
*/
class TempoEngine
{
public:
    static constexpr int maxTempos = 9;   // 0 = master, then up to 8 more (added as needed)
    enum TapValue { tapEighth = 0, tapQuarter, tapHalf, tapBar };
    enum MidiKind { midiNone = 0, midiNote, midiCC };
    enum OutWhat { outBeats = 0, outTaps };
    enum AudioMode { audioTrigger = 0, audioFollow };
    /** Where the audio of the audio taps comes from: a device input, or a rack (by id) at one of three points. */
    enum AudioPoint { pointDeviceInput = 0, pointPrePlugins, pointPostPlugins, pointPostFader };
    /** After a tap (or a tempo you set), the audio may only fine-tune within this much of it (default; per tempo: Config::audioWindowPct). */
    static constexpr double audioWindow = 0.04;
    enum Source { srcNone = 0, srcButton, srcKey, srcMidi, srcClock, srcAudio, srcManual, srcScene, srcOsc };
    enum OscLearn { oscLearnTap = 0, oscLearnBpm };

    static double tapValueBeats (int tapValue) noexcept;
    static juce::String tapValueName (int tapValue);
    static juce::String shortName (int index);   // "M", "T1".."T4"
    static juce::String sourceName (int source);

    struct Config
    {
        juce::String name;
        double ratio = 1.0;         // tempos 1..4: > 0 = follows the master x ratio, 0 = own tempo
        int tapValue = tapQuarter;  // what one tap is worth
        int keyCode = 0;            // computer key (juce key code), 0 = none
        int midiKind = midiNone, midiNumber = 0, midiChannel = 0;   // channel 0 = any
        bool midiClock = false;     // follow MIDI clock in (24 ppqn, Start / Song Position for the beat)
        int audioChannel = -1;      // device input channel, -1 = none
        int audioMode = audioTrigger;
        float audioThresholdDb = -30.0f;
        int audioPoint = pointDeviceInput;   // pointDeviceInput: audioChannel; otherwise the rack audioRackId
        int audioRackId = -1;
        /** Your tap wins over the audio. 0 = it stays until you tap again (the audio only fine-tunes
            within audioWindow); > 0 = if the audio keeps saying another tempo for this many seconds,
            the tempo goes back to the audio (the tap was a mistake). */
        int returnToAudioSec = 0;
        float audioWindowPct = 4.0f;   // tolerance around your tempo inside which the audio fine-tunes it (1-25 %)

        // --- sending this tempo out (TempoOutput): to a lighting desk, video, another computer...
        int outWhat = outBeats;     // outBeats: every beat of the tempo; outTaps: only your taps, as you give them
        juce::String midiOutId, midiOutName;   // MIDI output device (identifier; name to find it again)
        int midiOutKind = midiNone, midiOutNumber = 60, midiOutChannel = 1;   // 60 = C3
        bool midiOutClock = false;  // MIDI clock out (24 per beat, Start on a downbeat)
        bool oscOut = false;
        juce::String oscOutHost { "127.0.0.1" };
        int oscOutPort = 9000;
        juce::String oscOutBeat, oscOutBpm;   // addresses; empty = /stagerack/beat[/i], /stagerack/bpm[/i]

        bool sendsOut() const noexcept    { return (midiOutId.isNotEmpty() && (midiOutKind != midiNone || midiOutClock)) || oscOut; }
        bool sceneRecall = true;    // scenes store and recall this tempo
        juce::String oscTap;        // OSC address that taps (learned; no value or value > 0.5 = tap)
        juce::String oscBpm;        // OSC address that carries the tempo in BPM (learned)
    };

    struct Settings
    {
        int maxTaps = 8;
        double resetSec = 2.0;
        double roundStep = 0.0;     // 0 = no rounding, 0.5, 1
        double minBpm = 40.0, maxBpm = 240.0;   // taps outside are folded by octaves
        bool alignBeat = true;      // a tap also puts the beat (bar, with "tap = bar") on the tap
    };

    /** What the plugins of a rack see in the current device block (audio thread). */
    struct BlockInfo
    {
        double bpm = 120.0, ppq = 0.0, timeSec = 0.0;
        juce::int64 samplePos = 0;
        juce::uint64 hostTimeNs = 0;
    };

    TempoEngine();

    /** Tempos in use: the master + the ones added (message thread). */
    int getNumTempos() const noexcept              { return count.load(); }
    /** Adds a tempo (following the master x1); returns its index, -1 if all are used. */
    int addTempo();
    /** Removes tempo `index` (> 0): the ones after it move down one place. Racks and scenes
        are renumbered by the Session (Session::removeTempo). */
    void removeTempo (int index);

    //==============================================================================
    // message thread / MIDI thread
    Config getConfig (int index) const;
    void setConfig (int index, const Config&);
    Settings getSettings() const;
    void setSettings (const Settings&);
    bool isFollower (int index) const;

    double getBpm (int index) const;               // the tempo it plays now (follower: master x ratio)
    void setBpm (int index, double bpm, int source = srcManual);   // keeps the beat continuous; unlinks a follower
    void multiplyBpm (int index, double factor);   // x2, /2
    void link (int index, double ratio);           // follow the master again (ratio > 0)

    /** A tap at `timeSec` (seconds, Time::getMillisecondCounterHiRes base). A follower becomes
        independent (that is the point of tapping it). */
    void tap (int index, double timeSec, int source);
    void tapNow (int index, int source = srcButton)  { tap (index, nowSec(), source); }

    /** A key went down (repeats already filtered out): taps every tempo using it. */
    bool handleKey (int keyCode, double timeSec);
    bool isKeyUsed (int keyCode) const;
    /** MIDI input (any thread but the audio one): taps, clock, learn. */
    void handleMidi (const juce::MidiMessage&);
    /** Message thread, often (~30 Hz): turns the audio onsets into taps / beat tracking. */
    void poll()                                    { poll (nowSec()); }
    void poll (double now);                        // tests: simulated time

    /** OSC input (network thread). Fixed addresses (index 0 = master, 1.. = T1..; "m" works too):
          /stagerack/tap [/i]             tap (no value, or value > 0.5: a button's release is ignored)
          /stagerack/bpm [/i]  <bpm>      set the tempo
          /stagerack/beat [/i]            the beat is now (phase only)
          /stagerack/bar [/i]             the downbeat is now (phase only)
          /stagerack/half [/i], /double [/i], /follow [/i] <ratio>
        and /stagerack/tempo/<i>/tap ... (same commands). Any other address can be learned per
        tempo as its tap (a TouchOSC / Stream Deck / console button) or as its BPM (a number from
        QLab, Ableton, a lighting desk). Returns true if something used the message. */
    bool handleOsc (const juce::String& address, const std::vector<float>& values, double timeSec);
    /** Next OSC message becomes this tempo's tap (kind oscLearnTap) or BPM address; index -1 = cancel. */
    void learnOsc (int index, int kind);
    int getLearningOsc() const noexcept            { return oscLearning.load(); }
    int getLearningOscKind() const noexcept        { return oscLearningKind.load(); }

    /** Next MIDI note / CC (value > 63) becomes this tempo's tap; -1 = cancel. */
    void learnMidi (int index);
    int getLearningMidi() const noexcept           { return learning.load(); }

    double getBeatPosition (int index, double timeSec) const;   // ppq on the tempo's grid (beat LEDs)
    int getTapCount (int index) const;             // taps so far in the current series
    int getLastSource (int index) const;
    double getLastEventTime (int index) const;     // last tap / clock beat / tracked beat
    bool isClockRunning (int index) const;
    bool isTracking (int index) const;             // audio follow: locked on a beat
    /** What the audio says compared to the tempo you gave (tap / set): its tempo (0 = nothing yet),
        whether it is outside the window (ignored), and the seconds before it takes over (-1 = never). */
    struct AudioOpinion { double bpm = 0.0; bool differs = false; double takesOverIn = -1.0; bool anchored = false; };
    AudioOpinion getAudioOpinion (int index, double now = -1.0) const;
    float getTrackConfidence (int index) const;
    float readAudioLevel (int index) noexcept;     // detector envelope (peak since last read)
    juce::uint32 getOnsetCount (int index) const noexcept { return onsetCounts[(size_t) index].load (std::memory_order_relaxed); }
    /** Bumped when the configuration changes outside the UI (MIDI learn, session load). */
    juce::uint32 getChangeCount() const noexcept   { return changeCount.load(); }

    void resetToDefaults();
    juce::ValueTree toTree() const;
    void fromTree (const juce::ValueTree&);
    void storeInScene (juce::ValueTree& sceneData) const;
    void recallFromScene (const juce::ValueTree& sceneData);

    static double nowSec() noexcept                { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

    //==============================================================================
    // audio thread
    void prepare (double sampleRate);
    /** Start of every device callback, before the racks: grid -> beat position of this block
        for every tempo, audio onset detection. blockTimeSec: same base as nowSec(). */
    void processBlock (const float* const* deviceInputs, int numDeviceInputs, int numSamples,
                       double blockTimeSec, juce::uint64 hostTimeNs) noexcept;
    /** Audio taps from a rack (after the racks ran, IO thread): which rack / point tempo `index`
        listens to (-1 = a device input or none), then its samples (nullptr = no audio this block). */
    int getAudioRackId (int index) const noexcept  { return audioPoints[(size_t) index].load (std::memory_order_relaxed) != pointDeviceInput ? audioRackIds[(size_t) index].load (std::memory_order_relaxed) : -1; }
    int getAudioPoint (int index) const noexcept   { return audioPoints[(size_t) index].load (std::memory_order_relaxed); }
    void detectRackAudio (int index, const float* samples, int numSamples) noexcept;
    /** Lock-free read of a tempo's grid (any thread): ppq (t) = ppq + (t - time) * bpm / 60. */
    bool readGrid (int index, double& bpm, double& time, double& ppq) const noexcept;
    /** Taps you gave (button, key, MIDI, OSC: not the audio): count and time of the last one. */
    juce::uint32 getUserTapCount (int index) const noexcept { return userTaps[(size_t) index].load (std::memory_order_acquire); }
    const BlockInfo& getBlockInfo (int index) const noexcept { return blocks[(size_t) juce::jlimit (0, maxTempos - 1, index)]; }

private:
    struct ClockIn
    {
        std::array<double, 96> ticks {};
        int count = 0, head = 0;
        juce::int64 tickPos = -2;     // ticks since Start (-2 = beat unknown, -1 = Start received)
        juce::int64 ticksSeen = 0;
        double lastTick = -10.0;
    };

    struct State
    {
        Config cfg;
        double bpm = 120.0, gridTime = 0.0, gridPpq = 0.0;   // own grid: ppq (t) = gridPpq + (t - gridTime) * bpm / 60
        TapEstimator taps;
        BeatTracker tracker;
        double trackCandidate = 0.0, lastTrackEval = 0.0, lastOnset = 0.0;
        bool trackLocked = false;
        float trackConfidence = 0.0f;
        ClockIn clock;
        int lastSource = srcNone;
        double lastEvent = -10.0;
        TapEstimator audioTaps;          // "every hit = tap": the hits, apart from your taps
        double anchorBpm = 0.0;          // the tempo you gave (tap / set), 0 = none: the audio is free
        double audioBpm = 0.0, differsSince = -1.0, lastAudio = -10.0;
    };

    // lock-free grid for the audio thread (seqlock)
    struct Grid
    {
        std::atomic<juce::uint32> seq { 0 };
        std::atomic<double> bpm { 120.0 }, timeSec { 0.0 }, ppq { 0.0 };
    };

    double ppqAt (const State&, double t) const noexcept;
    void effectiveGrid (int index, double& bpm, double& time, double& ppq) const noexcept;
    void unlink (int index);                       // follower -> own, same tempo and beat
    void setOwnBpm (int index, double bpm, double atTime);
    void alignTo (int index, double beatTime, double unitBeats);
    double foldAndRound (double bpm) const noexcept;
    void publish();                                // lock held
    void tapLocked (int index, double timeSec, int source);
    void clockTick (int index, double t);
    void trackBeats (int index, double now);
    /** The audio's tempo and beat against your tap: applied, fine-tuned, ignored or taking over. */
    void audioSays (int index, double bpm, double beatTime, double unitBeats, double now);
    void detect (int index, const float* x, int numSamples) noexcept;
    TapEstimator::Settings tapSettings() const noexcept;
    void syncAudioConfig (int index);

    mutable std::mutex lock;
    std::array<State, maxTempos> st;
    Settings settings;
    std::atomic<int> learning { -1 };
    std::atomic<int> oscLearning { -1 }, oscLearningKind { oscLearnTap };
    void phaseLocked (int index, double timeSec, double unitBeats);
    std::atomic<int> count { 1 };
    void initTempo (int index, double now);
    std::atomic<juce::uint32> changeCount { 0 };

    std::array<Grid, maxTempos> grids;
    std::array<std::atomic<juce::uint32>, maxTempos> userTaps {};

    // audio thread
    struct Detector
    {
        float fast = 0.0f, slow = 0.0f;
        int sinceOnset = 1 << 30;
    };
    std::array<BlockInfo, maxTempos> blocks;
    std::array<double, maxTempos> acc {};
    std::array<juce::uint32, maxTempos> seenSeq {};
    bool firstBlock = true;
    std::array<Detector, maxTempos> detectors;
    std::array<std::atomic<int>, maxTempos> audioChannels, audioModes, audioPoints, audioRackIds;
    double blockStartSec = 0.0;
    std::array<std::atomic<float>, maxTempos> audioThresholds, audioLevels;
    std::array<std::atomic<juce::uint32>, maxTempos> onsetCounts;
    double sampleRate = 48000.0;
    juce::int64 samplePos = 0;
    float atkFast = 0, relFast = 0, atkSlow = 0, relSlow = 0;

    struct Onset { int index; double timeSec; };
    static constexpr int onsetCapacity = 256;
    juce::AbstractFifo onsetFifo { onsetCapacity };
    std::array<Onset, onsetCapacity> onsetRing {};

    JUCE_DECLARE_NON_COPYABLE (TempoEngine)
};

} // namespace stagerack
