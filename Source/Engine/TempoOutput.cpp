#include "TempoOutput.h"
#include "Features.h"
#include "OscInput.h"

namespace stagerack
{

TempoOutput::TempoOutput (TempoEngine& t, const OscInput& in) : tempo (t), oscIn (in) {}

TempoOutput::~TempoOutput()
{
    stopTimer();
    const juce::ScopedLock sl (lock);

    for (auto& o : outs)
        if (o.clockStarted && o.midi != nullptr)
            o.midi->sendMessageNow (juce::MidiMessage::midiStop());
}

juce::String TempoOutput::getProblem (int i) const
{
    const juce::ScopedLock sl (lock);
    return problems[(size_t) i];
}

static bool isThisMac (const juce::String& host)
{
    const auto h = host.trim().toLowerCase();
    if (h == "127.0.0.1" || h == "localhost" || h == "0.0.0.0" || h == "::1")
        return true;

    for (auto& a : juce::IPAddress::getAllAddresses())
        if (a.toString() == h)
            return true;

    return false;
}

void TempoOutput::update()
{
    const auto change = tempo.getChangeCount();
    const int n = tempo.getNumTempos();
    const bool listening = oscIn.isListening();
    const bool feature = features::has (features::tempo);   // off in Features: nothing goes out

    // nothing changed; a missing MIDI device is looked for again every ~3 s
    static int retry = 0;
    bool anyProblem = false;
    {
        const juce::ScopedLock sl (lock);
        for (int i = 0; i < n; ++i) anyProblem = anyProblem || problems[(size_t) i].isNotEmpty();
    }

    if (change == seenChange && n == seenCount && oscIn.getPort() == seenOscPort && listening == seenOscListening
        && feature == seenFeature && ! (anyProblem && ++retry % 90 == 0))
        return;

    seenFeature = feature;

    seenChange = change;
    seenCount = n;
    seenOscPort = oscIn.getPort();
    seenOscListening = listening;

    const auto devices = juce::MidiOutput::getAvailableDevices();
    std::array<TempoEngine::Config, TempoEngine::maxTempos> cfgs;
    for (int i = 0; i < n; ++i)
        cfgs[(size_t) i] = tempo.getConfig (i);

    const juce::ScopedLock sl (lock);
    bool anyActive = false;
    std::set<juce::String> usedMidi, usedOsc;

    for (int i = 0; i < TempoEngine::maxTempos; ++i)
    {
        auto& o = outs[(size_t) i];
        auto& problem = problems[(size_t) i];
        problem.clear();
        const auto& c = cfgs[(size_t) i];
        const bool wants = feature && i < n && c.sendsOut();

        juce::MidiOutput* midi = nullptr;
        juce::OSCSender* osc = nullptr;

        if (wants && c.midiOutId.isNotEmpty() && (c.midiOutKind != TempoEngine::midiNone || c.midiOutClock))
        {
            juce::String id;
            for (auto& d : devices) if (d.identifier == c.midiOutId) id = d.identifier;
            if (id.isEmpty()) for (auto& d : devices) if (d.name == c.midiOutName) id = d.identifier;

            if (id.isEmpty())
            {
                problem = "MIDI out \"" + c.midiOutName + "\" not connected";
            }
            else
            {
                auto& dev = midiDevices[id];
                if (dev == nullptr) dev = juce::MidiOutput::openDevice (id);
                if (dev == nullptr) problem = "MIDI out \"" + c.midiOutName + "\" can't be opened";
                else { midi = dev.get(); usedMidi.insert (id); }
            }
        }

        if (wants && c.oscOut)
        {
            if (listening && c.oscOutPort == oscIn.getPort() && isThisMac (c.oscOutHost))
            {
                problem = "OSC out to this Mac's own OSC in port: it would tap itself (loop)";
            }
            else
            {
                const auto key = c.oscOutHost.trim() + ":" + juce::String (c.oscOutPort);
                auto& snd = oscSenders[key];
                if (snd == nullptr)
                {
                    snd = std::make_unique<juce::OSCSender>();
                    if (! snd->connect (c.oscOutHost.trim(), c.oscOutPort))
                        snd.reset();
                }

                if (snd == nullptr) problem = "OSC out: can't send to " + key;
                else { osc = snd.get(); usedOsc.insert (key); }
            }
        }

        // what goes to the OLD destination before switching: the pending release (with the old
        // message kind), and Stop if the clock is turned off or moves to another device
        auto toOld = [&] (const juce::MidiMessage& m)
        {
            if (midiSinkForTests) midiSinkForTests (i, m);
            else if (o.midi != nullptr) o.midi->sendMessageNow (m);
        };
        const bool hadMidi = o.midi != nullptr || (midiSinkForTests != nullptr && o.cfg.midiOutId.isNotEmpty());
        const bool sameTarget = midi == o.midi && wants && c.midiOutId == o.cfg.midiOutId;

        if (o.noteOffAt >= 0.0 && hadMidi
            && ! (sameTarget && c.midiOutKind == o.cfg.midiOutKind && c.midiOutNumber == o.cfg.midiOutNumber && c.midiOutChannel == o.cfg.midiOutChannel))
        {
            if (o.cfg.midiOutKind == TempoEngine::midiNote)    toOld (juce::MidiMessage::noteOff (o.cfg.midiOutChannel, o.cfg.midiOutNumber));
            else if (o.cfg.midiOutKind == TempoEngine::midiCC) toOld (juce::MidiMessage::controllerEvent (o.cfg.midiOutChannel, o.cfg.midiOutNumber, 0));
            o.noteOffAt = -1.0;
        }

        if (o.clockStarted && hadMidi && ! (sameTarget && c.midiOutClock))
        {
            toOld (juce::MidiMessage::midiStop());
            o.clockStarted = false;
        }

        const bool wasActive = o.active;
        o.active = wants && (midi != nullptr || osc != nullptr || midiSinkForTests != nullptr);
        o.cfg = c;
        o.midi = midi;
        o.osc = osc;
        o.beatAddr = c.oscOutBeat.isNotEmpty() ? c.oscOutBeat : defaultBeatAddress (i);
        o.bpmAddr = c.oscOutBpm.isNotEmpty() ? c.oscOutBpm : defaultBpmAddress (i);
        if (! o.beatAddr.startsWith ("/")) o.beatAddr = "/" + o.beatAddr;
        if (! o.bpmAddr.startsWith ("/")) o.bpmAddr = "/" + o.bpmAddr;
        if (! wasActive) o.primed = false;
        o.lastBpmTime = -10.0;   // send the tempo again (new target, new address)
        anyActive = anyActive || o.active;
    }

    for (auto it = midiDevices.begin(); it != midiDevices.end();)
        it = usedMidi.count (it->first) == 0 ? midiDevices.erase (it) : std::next (it);
    for (auto it = oscSenders.begin(); it != oscSenders.end();)
        it = usedOsc.count (it->first) == 0 ? oscSenders.erase (it) : std::next (it);

    wantsThread = anyActive && manualTicksForTests;
    if (anyActive && ! manualTicksForTests && ! isTimerRunning()) startTimer (1);
    else if ((! anyActive || manualTicksForTests) && isTimerRunning()) stopTimer();
}

void TempoOutput::sendMidi (int i, const juce::MidiMessage& m)
{
    if (midiSinkForTests) midiSinkForTests (i, m);
    else if (auto* d = outs[(size_t) i].midi) d->sendMessageNow (m);
}

void TempoOutput::tick (double now)
{
    const juce::ScopedLock sl (lock);

    for (int i = 0; i < TempoEngine::maxTempos; ++i)
    {
        auto& o = outs[(size_t) i];
        if (! o.active)
            continue;

        double bpm, time, ppq;
        if (! tempo.readGrid (i, bpm, time, ppq))
            continue;

        const auto p = ppq + (now - time) * bpm / 60.0;
        const auto& c = o.cfg;
        const bool midiOn = (o.midi != nullptr || midiSinkForTests != nullptr) && c.midiOutId.isNotEmpty();

        if (! o.primed)
        {
            o.lastBeat = (juce::int64) std::floor (p);
            o.lastTick = (juce::int64) std::floor (p * 24.0);
            o.lastTaps = tempo.getUserTapCount (i);
            o.lastPpq = p;
            o.primed = true;
            continue;
        }

        auto fire = [&] (double at)
        {
            const auto b = (juce::int64) std::floor (at + 1.0e-6);
            const int inBar = (int) (((b % 4) + 4) % 4) + 1;

            if (midiOn && c.midiOutKind == TempoEngine::midiNote)
            {
                if (o.noteOffAt >= 0.0) sendMidi (i, juce::MidiMessage::noteOff (c.midiOutChannel, c.midiOutNumber));
                sendMidi (i, juce::MidiMessage::noteOn (c.midiOutChannel, c.midiOutNumber, (juce::uint8) 100));
                o.noteOffAt = now + 0.03;
            }
            else if (midiOn && c.midiOutKind == TempoEngine::midiCC)
            {
                sendMidi (i, juce::MidiMessage::controllerEvent (c.midiOutChannel, c.midiOutNumber, 127));
                o.noteOffAt = now + 0.03;
            }

            if (o.osc != nullptr && c.oscOut)
                o.osc->send (o.beatAddr, (juce::int32) inBar);

            sent[(size_t) i].fetch_add (1, std::memory_order_relaxed);
        };

        // the grid moved (a tap put the beat elsewhere): start counting again from here;
        // if the beat is right now (the tap that moved it), it is a beat
        const auto step = p - o.lastPpq;
        const bool jumped = step < -0.02 || step > 1.5;

        if (jumped)
        {
            o.lastBeat = (juce::int64) std::floor (p);
            o.lastTick = (juce::int64) std::floor (p * 24.0);
            if (c.outWhat == TempoEngine::outBeats && p - std::floor (p) < 0.05)
                fire (p);
        }
        else if (c.outWhat == TempoEngine::outBeats)
        {
            if (const auto b = (juce::int64) std::floor (p); b > o.lastBeat)
            {
                o.lastBeat = b;
                fire ((double) b);
            }
        }

        if (c.outWhat == TempoEngine::outTaps)
        {
            if (const auto taps = tempo.getUserTapCount (i); taps != o.lastTaps)
            {
                o.lastTaps = taps;
                fire (std::round (p));
            }
        }

        if (o.noteOffAt >= 0.0 && now >= o.noteOffAt)
        {
            if (midiOn && c.midiOutKind == TempoEngine::midiNote)     sendMidi (i, juce::MidiMessage::noteOff (c.midiOutChannel, c.midiOutNumber));
            else if (midiOn && c.midiOutKind == TempoEngine::midiCC)  sendMidi (i, juce::MidiMessage::controllerEvent (c.midiOutChannel, c.midiOutNumber, 0));
            o.noteOffAt = -1.0;
        }

        // MIDI clock: Start on a downbeat (the first clock after Start is beat 1), then 24 per beat
        if (midiOn && c.midiOutClock)
        {
            const auto t = (juce::int64) std::floor (p * 24.0);

            if (! o.clockStarted)
            {
                if (std::floor (p / 4.0) > std::floor (o.lastPpq / 4.0) && ! jumped)
                {
                    sendMidi (i, juce::MidiMessage::midiStart());
                    sendMidi (i, juce::MidiMessage::midiClock());
                    o.clockStarted = true;
                    o.lastTick = t;
                }
            }
            else if (t > o.lastTick)
            {
                const auto missed = juce::jmin ((juce::int64) 3, t - o.lastTick);   // a late tick catches up a little, a jump does not
                for (juce::int64 k = 0; k < missed; ++k)
                    sendMidi (i, juce::MidiMessage::midiClock());
                o.lastTick = t;
            }
            else if (t < o.lastTick)
            {
                o.lastTick = t;   // the grid went back (a tap): wait for it
            }
        }

        // the tempo itself over OSC: on change, and every 2 s
        if (o.osc != nullptr && c.oscOut && (std::abs (bpm - o.lastBpmSent) > 0.005 || now - o.lastBpmTime > 2.0))
        {
            o.osc->send (o.bpmAddr, (float) bpm);
            o.lastBpmSent = bpm;
            o.lastBpmTime = now;
        }

        o.lastPpq = p;
    }
}

} // namespace stagerack
