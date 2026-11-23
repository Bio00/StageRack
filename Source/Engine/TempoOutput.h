#pragma once

#include <JuceHeader.h>
#include <map>
#include <set>
#include "TempoEngine.h"

namespace stagerack
{

class OscInput;

/**
    Sends the tempos out, so a lighting desk, video, another computer or a drum machine follows
    StageRack's tap (per tempo, TempoEngine::Config::out* / midiOut* / oscOut*):

      - what: every BEAT of the tempo (continuous), or only YOUR TAPS as you give them (a desk's
        own tap key then gets your taps);
      - MIDI: a note (on, off after 30 ms) or a CC (127 then 0) on that event, and/or MIDI clock
        (24 per beat; Start on a downbeat, Stop when turned off);
      - OSC (UDP): <beat address> <beat in the bar 1-4> on the event, <bpm address> <bpm> when the
        tempo changes (and every 2 s for whoever joins later).

    A 1 ms timer thread reads the tempo grids lock-free (TempoEngine::readGrid); it runs only
    while some tempo sends out. Settings are taken on the message thread (update(), from the
    UI timer). OSC to this Mac's own OSC-in port is refused (it would tap itself in a loop).
*/
class TempoOutput final : private juce::HighResolutionTimer
{
public:
    TempoOutput (TempoEngine&, const OscInput&);
    ~TempoOutput() override;

    /** Message thread, often: picks up configuration changes, opens / closes devices, starts / stops the thread. */
    void update();

    /** The thread's work at time `now` (nowSec() base); public for the tests (simulated time). */
    void tick (double now);

    /** Tests: MIDI goes here instead of the devices (tempo index, message). */
    std::function<void (int, const juce::MidiMessage&)> midiSinkForTests;
    /** Tests: the 1 ms thread is not started, the test calls tick() with its own clock. */
    bool manualTicksForTests = false;

    /** UI: what the last update found wrong for tempo i (device missing, OSC loop...), empty if fine. */
    juce::String getProblem (int index) const;
    juce::uint32 getSentCount (int index) const noexcept { return sent[(size_t) index].load (std::memory_order_relaxed); }
    bool isRunning() const noexcept                    { return isTimerRunning() || wantsThread; }

    static juce::String defaultBeatAddress (int index)  { return index == 0 ? juce::String ("/stagerack/beat") : "/stagerack/beat/" + juce::String (index); }
    static juce::String defaultBpmAddress (int index)   { return index == 0 ? juce::String ("/stagerack/bpm") : "/stagerack/bpm/" + juce::String (index); }

private:
    void hiResTimerCallback() override                  { tick (TempoEngine::nowSec()); }
    void sendMidi (int index, const juce::MidiMessage&);

    struct Out
    {
        bool active = false;
        TempoEngine::Config cfg;
        juce::MidiOutput* midi = nullptr;
        juce::OSCSender* osc = nullptr;
        juce::String beatAddr, bpmAddr;

        // thread state
        double lastPpq = 0.0, lastBpmSent = 0.0, lastBpmTime = -10.0, noteOffAt = -1.0;
        juce::int64 lastBeat = 0, lastTick = 0;
        juce::uint32 lastTaps = 0;
        bool primed = false, clockStarted = false, clockWasOn = false;
    };

    TempoEngine& tempo;
    const OscInput& oscIn;
    juce::CriticalSection lock;   // outs + devices: message thread writes, timer thread reads
    std::array<Out, TempoEngine::maxTempos> outs;
    std::array<juce::String, TempoEngine::maxTempos> problems;
    std::array<std::atomic<juce::uint32>, TempoEngine::maxTempos> sent {};
    std::map<juce::String, std::unique_ptr<juce::MidiOutput>> midiDevices;
    std::map<juce::String, std::unique_ptr<juce::OSCSender>> oscSenders;
    juce::uint32 seenChange = 0xffffffffu;
    int seenCount = -1, seenOscPort = -1;
    bool seenOscListening = false, wantsThread = false;
    bool seenFeature = true;

    JUCE_DECLARE_NON_COPYABLE (TempoOutput)
};

} // namespace stagerack
