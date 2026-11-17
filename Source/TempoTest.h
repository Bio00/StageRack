#pragma once

#include "Engine/Session.h"

namespace stagerack
{

/** Tap tempo checks (part of --selftest). Times are given explicitly: no waiting, no device. */
inline void runTempoSelfTest (const std::function<void (bool, const juce::String&)>& check)
{
    juce::Random rng (1234);
    auto jitter = [&rng] (double ms) { return (rng.nextDouble() * 2.0 - 1.0) * ms * 0.001; };

    // --- estimator: least squares over the taps
    {
        TapEstimator est;
        TapEstimator::Settings s;
        const double period = 60.0 / 123.0;
        for (int k = 0; k < 8; ++k)
            est.tap (100.0 + k * period + jitter (15.0), s);

        const auto bpm = 60.0 / est.getPeriod();
        check (std::abs (bpm - 123.0) < 1.5, "tap: 8 taps at 123 BPM, +-15 ms jitter -> " + juce::String (bpm, 2));

        const auto bounced = est.tap (100.0 + 7 * period + 0.03, s);
        check (! bounced, "tap: a bounce 30 ms after a tap is ignored");

        // a clearly slower tempo starts a new series
        double t = 100.0 + 8 * period;
        for (int k = 0; k < 4; ++k)
            est.tap (t + k * 0.75, s);
        check (std::abs (60.0 / est.getPeriod() - 80.0) < 0.5 && est.getNumTaps() == 4,
               "tap: a new tempo (80) starts a new series (" + juce::String (60.0 / est.getPeriod(), 2) + ", " + juce::String (est.getNumTaps()) + " taps)");

        est.tap (t + 3 * 0.75 + 5.0, s);
        check (est.getNumTaps() == 1, "tap: a long pause starts over");
    }

    // --- engine: master, followers, tap value, folding, beat alignment
    {
        TempoEngine te;
        check (te.getNumTempos() == 1, "tempo: a new engine has only the master");
        for (int k = 0; k < 4; ++k) te.addTempo();
        const double base = 1000.0;
        for (int k = 0; k < 6; ++k)
            te.tap (0, base + k * 0.5, TempoEngine::srcButton);

        check (std::abs (te.getBpm (0) - 120.0) < 0.01, "tempo: master tapped at 120 -> " + juce::String (te.getBpm (0), 3));
        check (std::abs (te.getBpm (1) - 120.0) < 0.01 && te.isFollower (1), "tempo: T1 follows the master (= master)");

        te.link (2, 2.0);
        check (std::abs (te.getBpm (2) - 240.0) < 0.01, "tempo: T2 = master x2");

        const auto ppq = te.getBeatPosition (0, base + 5 * 0.5);
        check (std::abs (ppq - std::round (ppq)) < 1.0e-6, "tempo: the beat sits on the last tap (ppq " + juce::String (ppq, 4) + ")");
        const auto ppq2 = te.getBeatPosition (2, base + 5 * 0.5);
        check (std::abs (ppq2 - 2.0 * ppq) < 1.0e-6, "tempo: a x2 follower keeps the master's beat (ppq x2)");

        // tapping a follower makes it independent
        for (int k = 0; k < 4; ++k)
            te.tap (1, base + 10.0 + k * (60.0 / 90.0), TempoEngine::srcKey);
        check (! te.isFollower (1) && std::abs (te.getBpm (1) - 90.0) < 0.01 && std::abs (te.getBpm (0) - 120.0) < 0.01,
               "tempo: tapping T1 unlinks it (90), master stays 120");

        // tap = bar: taps on the downbeats
        auto c = te.getConfig (3);
        c.tapValue = TempoEngine::tapBar;
        te.setConfig (3, c);
        for (int k = 0; k < 4; ++k)
            te.tap (3, base + 20.0 + k * 1.6, TempoEngine::srcMidi);
        const auto barPpq = te.getBeatPosition (3, base + 20.0 + 3 * 1.6);
        check (std::abs (te.getBpm (3) - 150.0) < 0.01 && std::abs (std::fmod (barPpq, 4.0)) < 1.0e-6,
               "tempo: tap = bar, a tap every 1.6 s -> 150 BPM, downbeat on the tap (ppq " + juce::String (barPpq, 3) + ")");

        // folding into the range
        for (int k = 0; k < 5; ++k)
            te.tap (4, base + 40.0 + k * 0.2, TempoEngine::srcButton);   // 300 BPM
        check (std::abs (te.getBpm (4) - 150.0) < 0.01, "tempo: 300 BPM taps fold to 150 (range 40-240)");

        te.multiplyBpm (4, 0.5);
        check (std::abs (te.getBpm (4) - 75.0) < 0.01, "tempo: /2");

        auto st = te.getSettings();
        st.roundStep = 1.0;
        te.setSettings (st);
        for (int k = 0; k < 4; ++k)
            te.tap (0, base + 60.0 + k * (60.0 / 97.4), TempoEngine::srcButton);
        check (std::abs (te.getBpm (0) - 97.0) < 1.0e-9, "tempo: round to 1 BPM (97.4 -> " + juce::String (te.getBpm (0), 2) + ")");
    }

    // --- keys: one tap per key; MIDI learn + note taps; MIDI clock
    {
        TempoEngine te;
        for (int k = 0; k < 2; ++k) te.addTempo();
        check (te.isKeyUsed ('t') && te.isKeyUsed ('T') && ! te.isKeyUsed ('x'), "tempo: key T taps the master by default");
        for (int k = 0; k < 4; ++k)
            te.handleKey ('t', 500.0 + k * 0.6);
        check (std::abs (te.getBpm (0) - 100.0) < 0.01, "tempo: key taps -> 100 BPM");

        te.learnMidi (2);
        juce::MidiMessage learn = juce::MidiMessage::noteOn (10, 36, (juce::uint8) 100);
        learn.setTimeStamp (TempoEngine::nowSec());
        te.handleMidi (learn);
        const auto c = te.getConfig (2);
        check (c.midiKind == TempoEngine::midiNote && c.midiNumber == 36 && c.midiChannel == 10 && te.getLearningMidi() < 0,
               "tempo: MIDI learn takes note 36 ch10");

        const auto now = TempoEngine::nowSec() - 3.0;
        const double period = 60.0 / 140.0;
        for (int k = 0; k < 5; ++k)
        {
            auto m = juce::MidiMessage::noteOn (10, 36, (juce::uint8) 90);
            m.setTimeStamp (now + k * period);
            te.handleMidi (m);
            auto off = juce::MidiMessage::noteOff (10, 36);
            off.setTimeStamp (now + k * period + 0.05);
            te.handleMidi (off);
        }
        check (std::abs (te.getBpm (2) - 140.0) < 0.01, "tempo: MIDI note taps (timestamps) -> " + juce::String (te.getBpm (2), 3));

        auto cc = te.getConfig (1);
        cc.midiClock = true;
        cc.ratio = 0.0;
        te.setConfig (1, cc);
        const auto t0 = TempoEngine::nowSec() - 3.0;
        const double tick = 60.0 / (128.0 * 24.0);
        auto start = juce::MidiMessage::midiStart();
        start.setTimeStamp (t0 - 0.001);
        te.handleMidi (start);
        for (int k = 0; k < 24 * 6; ++k)
        {
            auto m = juce::MidiMessage::midiClock();
            m.setTimeStamp (t0 + k * tick + jitter (1.0));
            te.handleMidi (m);
        }
        const auto beat = te.getBeatPosition (1, t0 + 24 * 5 * tick);
        check (std::abs (te.getBpm (1) - 128.0) < 0.1 && std::abs (beat - 5.0) < 0.02,
               "tempo: MIDI clock 128 BPM (+-1 ms) -> " + juce::String (te.getBpm (1), 3) + ", beat 5 at the 120th tick (" + juce::String (beat, 3) + ")");
    }

    // --- audio taps: a click track on an input, detector on the audio thread
    {
        TempoEngine te;
        const double sr = 48000.0;
        const int bs = 32;
        te.prepare (sr);
        auto c = te.getConfig (0);
        c.audioChannel = 1;
        c.audioMode = TempoEngine::audioTrigger;
        c.audioThresholdDb = -30.0f;
        te.setConfig (0, c);

        const double bpm = 104.0, period = 60.0 / bpm;
        const auto t0 = TempoEngine::nowSec() - 4.0;
        std::vector<float> ch0 ((size_t) bs, 0.0f), ch1 ((size_t) bs, 0.0f);
        const float* ins[2] = { ch0.data(), ch1.data() };
        juce::int64 pos = 0;

        for (int b = 0; b < (int) (3.5 * sr / bs); ++b)
        {
            for (int n = 0; n < bs; ++n)
            {
                const auto t = (double) (pos + n) / sr;
                const auto since = std::fmod (t, period);
                ch1[(size_t) n] = since < 0.004 ? 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 2000.0 * since) : 0.0005f;
            }

            te.processBlock (ins, 2, bs, t0 + (double) pos / sr, 0);
            pos += bs;
        }

        te.poll();
        check (std::abs (te.getBpm (0) - bpm) < 0.1 && te.getOnsetCount (0) >= 5,
               "tempo: click track on IN 2 -> " + juce::String (te.getBpm (0), 3) + " BPM (" + juce::String ((int) te.getOnsetCount (0)) + " hits)");
    }

    // --- beat tracking from kick onsets (syncopated, some hits missing, +-6 ms)
    {
        auto pattern = [&] (double bpm, std::initializer_list<double> beatsInBar, double centre)
        {
            BeatTracker bt;
            const double p = 60.0 / bpm;
            const double t0 = 50.0;
            double last = 0.0;
            for (int bar = 0; bar < 4; ++bar)
                for (auto b : beatsInBar)
                {
                    if (rng.nextFloat() < 0.1f) continue;   // missed hit
                    last = t0 + (bar * 4 + b) * p + jitter (6.0);
                    bt.addOnset (last);
                }

            BeatTracker::Result r;
            const auto ok = bt.estimate (last + 0.1, centre, r);
            return std::pair<bool, BeatTracker::Result> (ok, r);
        };

        auto rock = pattern (126.0, { 0.0, 1.5, 2.0, 2.5 }, 120.0);
        check (rock.first && std::abs (rock.second.bpm - 126.0) < 0.5,
               "beat tracking: rock kick (1, 2&, 3, 3&) at 126 -> " + juce::String (rock.second.bpm, 2) + " (confidence " + juce::String (rock.second.confidence, 2) + ")");

        auto four = pattern (95.0, { 0.0, 1.0, 2.0, 3.0 }, 120.0);
        check (four.first && std::abs (four.second.bpm - 95.0) < 0.5, "beat tracking: four on the floor at 95 -> " + juce::String (four.second.bpm, 2));

        auto half = pattern (140.0, { 0.0, 2.0 }, 140.0);
        check (half.first && std::abs (half.second.bpm - 140.0) < 0.5, "beat tracking: kick on 1 and 3 at 140 stays at 140 (no half time) -> " + juce::String (half.second.bpm, 2));
    }

    // --- the plugins of a rack see its tempo through the play head; session and scenes
    {
        struct Probe final : juce::AudioProcessorGraph::AudioGraphIOProcessor
        {
            Probe() : AudioGraphIOProcessor (audioInputNode) {}
            void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
            {
                if (auto* ph = getPlayHead())
                    if (auto pos = ph->getPosition())
                    {
                        bpm = pos->getBpm().orFallback (0.0);
                        previousPpq = ppq;
                        ppq = pos->getPpqPosition().orFallback (-1.0);
                        playing = pos->getIsPlaying();
                    }
                ++calls;
            }
            double bpm = 0.0, ppq = -1.0, previousPpq = -1.0;
            bool playing = false;
            int calls = 0;
        };

        AudioEngine e;
        e.tempo.addTempo();
        e.tempo.addTempo();
        auto& r = e.addRack();
        auto probe = std::make_unique<Probe>();
        auto* p = probe.get();
        r.setPlugin (0, std::move (probe));
        r.tempoIndex = 2;
        e.tempo.link (2, 0.5);
        e.tempo.setBpm (0, 128.0);
        e.prepare (48000.0, 64, {});
        p->calls = 0;   // the hop alignment ran it offline

        juce::AudioBuffer<float> in (2, 64), out (2, 64);
        in.clear();
        juce::AudioIODeviceCallbackContext ctx;
        for (int k = 0; k < 3; ++k)
            e.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 2, 64, ctx);

        const auto step = 64.0 / 48000.0 * 64.0 / 60.0;
        check (p->calls == 3 && std::abs (p->bpm - 64.0) < 1.0e-9 && p->playing && std::abs ((p->ppq - p->previousPpq) - step) < step * 0.1,
               "play head: rack on T2 (master 128 x1/2) -> plugin sees " + juce::String (p->bpm, 2) + " BPM, playing, beat advancing "
                 + juce::String (p->ppq - p->previousPpq, 5) + " per block (" + juce::String (step, 5) + ")");

        auto removed = r.setPlugin (0, nullptr);
        check (removed != nullptr && removed->getPlayHead() == nullptr, "play head: a plugin taken out of a rack loses it");
        e.audioDeviceStopped();

        // session file and scenes
        auto* pm = new PluginManager();   // leaked on purpose: its destructor saves files
        AudioEngine e1;
        Session s (e1, *pm);
        s.newSession();
        for (int k = 0; k < 3; ++k) s.addTempo();
        e1.getRack (0).tempoIndex = 3;
        auto c = e1.tempo.getConfig (3);
        c.name = "Vox delay";
        c.keyCode = 'y';
        c.midiKind = TempoEngine::midiCC;
        c.midiNumber = 64;
        c.audioChannel = 7;
        c.audioMode = TempoEngine::audioFollow;
        e1.tempo.setConfig (3, c);
        e1.tempo.setBpm (3, 93.5);
        e1.tempo.setBpm (0, 110.0);
        s.addScene ("Song A");
        e1.tempo.setBpm (0, 76.0);
        s.addScene ("Song B");

        AudioEngine e2;
        Session s2 (e2, *pm);
        s2.loadFromTree (s.toTree(), {}, false);
        const auto c2 = e2.tempo.getConfig (3);
        check (e2.getRack (0).tempoIndex == 3 && c2.name == "Vox delay" && juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) c2.keyCode) == 'y'
                 && c2.midiKind == TempoEngine::midiCC && c2.midiNumber == 64 && c2.audioChannel == 7 && c2.audioMode == TempoEngine::audioFollow
                 && std::abs (e2.tempo.getBpm (3) - 93.5) < 1.0e-6 && std::abs (e2.tempo.getBpm (0) - 76.0) < 1.0e-6,
               "session keeps the rack's tempo, the tempos and their tap sources");

        s2.recallScene (0);
        check (std::abs (e2.tempo.getBpm (0) - 110.0) < 1.0e-6 && std::abs (e2.tempo.getBpm (1) - 110.0) < 1.0e-6,
               "scene recall sets the song tempo (Song A: 110, T1 follows)");
        s2.recallScene (1);
        check (std::abs (e2.tempo.getBpm (0) - 76.0) < 1.0e-6, "scene recall: Song B 76");

        // removing T1: T3 becomes T2 (rack and scenes follow)
        auto& r2 = e2.addRack();
        r2.tempoIndex = 1;
        e2.tempo.setBpm (3, 101.0);
        s2.storeScene (1);
        s2.removeTempo (1);
        check (e2.tempo.getNumTempos() == 3 && e2.getRack (0).tempoIndex == 2 && r2.tempoIndex == 0
                 && e2.tempo.getConfig (2).name == "Vox delay" && std::abs (e2.tempo.getBpm (2) - 101.0) < 1.0e-6,
               "remove T1: T3 -> T2 with its racks, T1's racks -> master");
        e2.tempo.setBpm (2, 60.0);
        s2.recallScene (1);
        check (std::abs (e2.tempo.getBpm (2) - 101.0) < 1.0e-6, "remove T1: scenes renumbered too (T2 recalls 101)");

        s2.newSession();
        check (std::abs (e2.tempo.getBpm (0) - 120.0) < 1.0e-6 && e2.tempo.getNumTempos() == 1, "new session: only the master tempo, 120");
    }

    // --- OSC: fixed addresses, learn, release ignored, persistence; then real UDP packets
    {
        TempoEngine te;
        te.addTempo();
        te.link (1, 0.0);   // T1 own tempo
        auto cfg = te.getConfig (1); cfg.ratio = 0.0; te.setConfig (1, cfg);
        const std::vector<float> none, one { 1.0f }, zero { 0.0f };
        double t = 500.0;
        const double p = 60.0 / 100.0;

        for (int k = 0; k < 6; ++k) te.handleOsc ("/stagerack/tap", none, t + k * p);
        check (std::abs (te.getBpm (0) - 100.0) < 0.05, "osc: /stagerack/tap x6 at 100 -> master " + juce::String (te.getBpm (0), 2));

        for (int k = 0; k < 6; ++k) { te.handleOsc ("/stagerack/tempo/1/tap", one, 600.0 + k * 0.5); te.handleOsc ("/stagerack/tempo/1/tap", zero, 600.0 + k * 0.5 + 0.1); }
        check (std::abs (te.getBpm (1) - 120.0) < 0.05 && std::abs (te.getBpm (0) - 100.0) < 0.05,
               "osc: /stagerack/tempo/1/tap press 1 / release 0 -> T1 120 (" + juce::String (te.getBpm (1), 2) + "), master untouched");

        te.handleOsc ("/stagerack/bpm/1", { 87.5f }, 700.0);
        te.handleOsc ("/stagerack/bpm", { 999.0f }, 700.0);   // out of range: ignored
        check (std::abs (te.getBpm (1) - 87.5) < 1.0e-6 && std::abs (te.getBpm (0) - 100.0) < 0.05, "osc: /stagerack/bpm/1 87.5, 999 ignored");

        te.handleOsc ("/stagerack/double/1", none, 701.0);
        check (std::abs (te.getBpm (1) - 175.0) < 1.0e-6, "osc: /stagerack/double/1");

        te.handleOsc ("/stagerack/bar", none, 702.0);
        check (std::abs (te.getBeatPosition (0, 702.0) - std::round (te.getBeatPosition (0, 702.0) / 4.0) * 4.0) < 1.0e-6,
               "osc: /stagerack/bar puts the downbeat now");

        te.handleOsc ("/stagerack/follow/1", { 0.5f }, 703.0);
        check (te.isFollower (1) && std::abs (te.getBpm (1) - 50.0) < 0.05, "osc: /stagerack/follow/1 0.5 -> follows the master x1/2");

        te.learnOsc (1, TempoEngine::oscLearnTap);
        te.handleOsc ("/1/push3", zero, 800.0);   // a release does not learn
        te.handleOsc ("/1/push3", one, 800.0);
        te.learnOsc (0, TempoEngine::oscLearnBpm);
        te.handleOsc ("/btn", none, 800.0);       // no number: still learning
        te.handleOsc ("/live/tempo", { 128.0f }, 800.0);
        check (te.getConfig (1).oscTap == "/1/push3" && te.getConfig (0).oscBpm == "/live/tempo" && te.getLearningOsc() == -1,
               "osc learn: tap /1/push3 on T1 (release skipped), BPM /live/tempo on the master");

        te.handleOsc ("/live/tempo", { 131.0f }, 801.0);
        for (int k = 0; k < 6; ++k) te.handleOsc ("/1/push3", one, 900.0 + k * 0.4);
        check (std::abs (te.getBpm (0) - 131.0) < 1.0e-6 && std::abs (te.getBpm (1) - 150.0) < 0.05,
               "osc learned: /live/tempo 131 -> master, /1/push3 taps -> T1 150 (" + juce::String (te.getBpm (1), 2) + ")");

        TempoEngine te2;
        te2.fromTree (te.toTree());
        check (te2.getConfig (1).oscTap == "/1/push3" && te2.getConfig (0).oscBpm == "/live/tempo", "osc: learned addresses saved with the session");

        // real UDP: OSCSender -> OscInput (network thread) -> tempo
        AudioEngine e;
        const int port = 47000 + juce::Random::getSystemRandom().nextInt (2000);
        const bool open = e.osc.setEnabled (true, port);
        juce::OSCSender sender;
        const bool connected = sender.connect ("127.0.0.1", port);
        sender.send ("/stagerack/bpm", 142.0f);
        sender.send (juce::OSCMessage ("/stagerack/tap"));

        for (int w = 0; w < 100 && e.osc.getMessageCount() < 2; ++w)
            juce::Thread::sleep (10);

        check (open && connected && e.osc.getMessageCount() == 2 && std::abs (e.tempo.getBpm (0) - 142.0) < 1.0e-6
                 && e.tempo.getLastSource (0) == TempoEngine::srcOsc,
               "osc over UDP (port " + juce::String (port) + "): " + juce::String (e.osc.getMessageCount()) + " messages, master "
                 + juce::String (e.tempo.getBpm (0), 2));

        juce::OSCReceiver blocker;
        const bool taken = blocker.connect (port + 1);
        check (! taken || ! e.osc.setEnabled (true, port + 1), "osc: a busy port is reported");
        e.osc.setEnabled (false, port);
    }

    // --- your tap wins over the audio: fine-tune within 4%, ignore beyond, or give back after N s
    {
        const double sr = 48000.0;
        const int bs = 32;
        const auto tau = juce::MathConstants<double>::twoPi;

        // drums (a click on every beat) on IN 2 from `from` for `secs`, poll every 0.25 s (simulated time)
        auto drums = [&] (TempoEngine& te, double bpm, double from, double secs)
        {
            std::vector<float> ch0 ((size_t) bs, 0.0f), ch1 ((size_t) bs, 0.0f);
            const float* ins[2] = { ch0.data(), ch1.data() };
            const double period = 60.0 / bpm;
            double nextPoll = from;

            for (juce::int64 pos = 0; pos < (juce::int64) (secs * sr); pos += bs)
            {
                for (int n = 0; n < bs; ++n)
                {
                    const auto since = std::fmod ((double) (pos + n) / sr, period);
                    ch1[(size_t) n] = since < 0.004 ? 0.5f * (float) std::sin (tau * 2000.0 * since) : 0.0005f;
                }

                const auto t = from + (double) pos / sr;
                te.processBlock (ins, 2, bs, t, 0);
                if (t >= nextPoll) { te.poll (t); nextPoll += 0.25; }
            }

            return from + secs;
        };

        auto makeEngine = [&] (int mode, int returnSec)
        {
            auto te = std::make_unique<TempoEngine>();
            te->prepare (sr);
            auto c = te->getConfig (0);
            c.audioChannel = 1;
            c.audioMode = mode;
            c.returnToAudioSec = returnSec;
            te->setConfig (0, c);
            return te;
        };

        auto tapAt = [] (TempoEngine& te, double bpm, double from, int n)
        {
            for (int k = 0; k < n; ++k)
                te.tap (0, from + k * 60.0 / bpm, TempoEngine::srcButton);
            return from + n * 60.0 / bpm;
        };

        const auto t0 = TempoEngine::nowSec();

        {
            auto te = makeEngine (TempoEngine::audioFollow, 0);
            const auto end = drums (*te, 128.0, tapAt (*te, 96.0, t0, 4), 12.0);
            const auto o = te->getAudioOpinion (0, end);
            check (std::abs (te->getBpm (0) - 96.0) < 0.05 && o.differs && std::abs (o.bpm - 128.0) < 1.0 && o.takesOverIn < 0.0,
                   "tap wins: tapped 96, drums at 128 for 12 s -> stays " + juce::String (te->getBpm (0), 2) + " (audio says " + juce::String (o.bpm, 1) + ", ignored)");
        }
        {
            auto te = makeEngine (TempoEngine::audioFollow, 0);
            const auto start = tapAt (*te, 96.0, t0, 4);
            const auto end = drums (*te, 98.5, start, 8.0);
            // the drums' beats are at start + k * period: is the grid on them?
            const auto beat = start + 13 * 60.0 / 98.5;
            const auto p = te->getBeatPosition (0, beat);
            check (std::abs (te->getBpm (0) - 98.5) < 0.3 && std::abs (p - std::round (p)) < 0.1 && ! te->getAudioOpinion (0, end).differs,
                   "tap + audio: tapped 96, drums at 98.5 (within 4%) -> fine-tuned to " + juce::String (te->getBpm (0), 2)
                     + ", beat on the drums (" + juce::String (p - std::round (p), 3) + ")");
        }
        {
            auto te = makeEngine (TempoEngine::audioFollow, 8);
            const auto start = tapAt (*te, 96.0, t0, 4);
            auto t = drums (*te, 128.0, start, 6.0);
            const auto mid = te->getBpm (0);
            const auto o = te->getAudioOpinion (0, t);
            drums (*te, 128.0, t, 8.0);
            check (std::abs (mid - 96.0) < 0.05 && o.takesOverIn > 0.0 && std::abs (te->getBpm (0) - 128.0) < 0.5,
                   "back to the audio after 8 s: 6 s -> " + juce::String (mid, 2) + " (takes over in " + juce::String (o.takesOverIn, 1) + " s), 14 s -> "
                     + juce::String (te->getBpm (0), 2));
        }
        {
            auto te = makeEngine (TempoEngine::audioTrigger, 0);
            const auto end = drums (*te, 140.0, tapAt (*te, 100.0, t0, 4), 5.0);
            const auto o = te->getAudioOpinion (0, end);
            check (std::abs (te->getBpm (0) - 100.0) < 0.05 && o.differs && std::abs (o.bpm - 140.0) < 1.0,
                   "every hit = tap: your taps (100) are not mixed with the hits (140) -> " + juce::String (te->getBpm (0), 2));
        }
        {
            auto te = makeEngine (TempoEngine::audioTrigger, 0);
            drums (*te, 140.0, t0, 5.0);
            check (std::abs (te->getBpm (0) - 140.0) < 0.1, "every hit = tap, no tap of yours: the hits set it -> " + juce::String (te->getBpm (0), 2));
        }
        {
            // tolerance per tempo: drums 8% off the tap -> ignored at 4%, fine-tuned at 10%
            auto te = makeEngine (TempoEngine::audioFollow, 0);
            auto c = te->getConfig (0); c.audioWindowPct = 10.0f; te->setConfig (0, c);
            drums (*te, 104.0, tapAt (*te, 96.0, t0, 4), 8.0);
            auto te4 = makeEngine (TempoEngine::audioFollow, 0);
            drums (*te4, 104.0, tapAt (*te4, 96.0, t0, 4), 8.0);
            check (std::abs (te->getBpm (0) - 104.0) < 0.3 && std::abs (te4->getBpm (0) - 96.0) < 0.05,
                   "tolerance: drums 8% off the tap -> +-10%: " + juce::String (te->getBpm (0), 2) + ", +-4%: " + juce::String (te4->getBpm (0), 2));
        }
    }

    // --- audio taken from a rack: before the plugins / after them / after the fader
    {
        const double sr = 48000.0;
        const int bs = 32;
        AudioEngine e;
        auto& r = e.addRack();
        r.inputChannel = 0; r.stereoInput = false; r.outputChannel = 0;
        e.prepare (sr, bs, {});

        auto c = e.tempo.getConfig (0);
        c.audioPoint = TempoEngine::pointPostFader;
        c.audioRackId = r.getId();
        c.audioMode = TempoEngine::audioTrigger;
        e.tempo.setConfig (0, c);

        juce::AudioBuffer<float> in (2, bs), out (4, bs);
        juce::AudioIODeviceCallbackContext ctx;
        juce::uint64 host = (juce::uint64) (TempoEngine::nowSec() * 1.0e9);
        ctx.hostTimeNs = &host;
        juce::int64 pos = 0;
        const double period = 60.0 / 120.0;

        auto run = [&] (double secs)
        {
            double nextPoll = 0.0;
            for (juce::int64 k = 0; k < (juce::int64) (secs * sr); k += bs)
            {
                for (int n = 0; n < bs; ++n)
                {
                    const auto since = std::fmod ((double) (pos + n) / sr, period);
                    in.setSample (0, n, since < 0.004 ? 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 2000.0 * since) : 0.0005f);
                }
                e.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 4, bs, ctx);
                const auto t = (double) host * 1.0e-9;
                if ((double) k / sr >= nextPoll) { e.tempo.poll (t); nextPoll += 0.25; }
                host += (juce::uint64) std::llround (bs / sr * 1.0e9);
                pos += bs;
            }
        };

        run (4.0);
        check (std::abs (e.tempo.getBpm (0) - 120.0) < 0.1 && e.tempo.getOnsetCount (0) >= 6,
               "rack post fader: click through the rack -> " + juce::String (e.tempo.getBpm (0), 2) + " (" + juce::String ((int) e.tempo.getOnsetCount (0)) + " hits)");

        r.muted = true;
        run (0.2);   // fader ramp
        const auto before = e.tempo.getOnsetCount (0);
        run (2.0);
        const auto mutedHits = e.tempo.getOnsetCount (0) - before;

        c.audioPoint = TempoEngine::pointPrePlugins;
        e.tempo.setConfig (0, c);
        const auto before2 = e.tempo.getOnsetCount (0);
        run (2.0);
        const auto preHits = e.tempo.getOnsetCount (0) - before2;
        check (mutedHits == 0 && preHits >= 3, "rack muted: post fader hears nothing (" + juce::String ((int) mutedHits) + " hits), before the plugins still does ("
                                                    + juce::String ((int) preHits) + ")");

        c.audioRackId = 9999;   // rack removed
        e.tempo.setConfig (0, c);
        const auto before3 = e.tempo.getOnsetCount (0);
        run (1.0);
        check (e.tempo.getOnsetCount (0) == before3, "rack source removed: nothing, no crash");

        c.audioRackId = r.getId();
        c.audioPoint = TempoEngine::pointPostPlugins;
        c.returnToAudioSec = 16;
        e.tempo.setConfig (0, c);
        TempoEngine back;
        back.fromTree (e.tempo.toTree());
        const auto bc = back.getConfig (0);
        check (bc.audioPoint == TempoEngine::pointPostPlugins && bc.audioRackId == r.getId() && bc.returnToAudioSec == 16,
               "session keeps the audio source (rack, point) and \"after a tap\"");
    }

    // --- sending the tempo out: MIDI note / CC / clock (simulated time), OSC over real UDP
    {
        AudioEngine e;
        auto& te = e.tempo;
        te.setBpm (0, 120.0);
        std::vector<juce::MidiMessage> got;
        e.tempoOut.midiSinkForTests = [&got] (int, const juce::MidiMessage& m) { got.push_back (m); };
        e.tempoOut.manualTicksForTests = true;

        auto c = te.getConfig (0);
        c.midiOutId = "test"; c.midiOutName = "test";
        c.midiOutKind = TempoEngine::midiNote; c.midiOutNumber = 60; c.midiOutChannel = 10; c.midiOutClock = true;
        te.setConfig (0, c);
        e.tempoOut.update();

        const auto t0 = TempoEngine::nowSec();
        for (int ms = 0; ms <= 4000; ++ms)
            e.tempoOut.tick (t0 + ms * 0.001);

        int notesOn = 0, notesOff = 0, clocks = 0, starts = 0;
        for (auto& m : got)
        {
            notesOn += m.isNoteOn() && m.getNoteNumber() == 60 && m.getChannel() == 10 ? 1 : 0;
            notesOff += m.isNoteOff() ? 1 : 0;
            clocks += m.isMidiClock() ? 1 : 0;
            starts += m.isMidiStart() ? 1 : 0;
        }
        // 4 s at 120 = 8 beats; clock starts on the next downbeat (within 2 s), then 24 per beat
        check (notesOn >= 7 && notesOn <= 9 && notesOff >= notesOn - 1 && notesOff <= notesOn && starts == 1 && clocks >= 24 * 4 - 2 && clocks <= 24 * 8 + 2,
               "send MIDI: 4 s at 120 -> " + juce::String (notesOn) + " notes (ch10 C3), " + juce::String (starts) + " Start, " + juce::String (clocks) + " clocks");

        // "my taps": only the taps go out, when they come
        got.clear();
        c.outWhat = TempoEngine::outTaps; c.midiOutClock = false; c.midiOutKind = TempoEngine::midiCC; c.midiOutNumber = 20;
        te.setConfig (0, c);
        e.tempoOut.update();
        const auto t1 = t0 + 5.0;
        for (int ms = 0; ms <= 3000; ++ms)
        {
            const auto t = t1 + ms * 0.001;
            if (ms == 500 || ms == 1000 || ms == 1500) te.tap (0, t, TempoEngine::srcButton);
            e.tempoOut.tick (t);
        }
        int cc127 = 0, cc0 = 0;
        for (auto& m : got) { cc127 += m.isController() && m.getControllerNumber() == 20 && m.getControllerValue() == 127 ? 1 : 0;
                              cc0 += m.isController() && m.getControllerValue() == 0 ? 1 : 0; }
        check (cc127 == 3 && cc0 == 3, "send MIDI, my taps only: 3 taps -> " + juce::String (cc127) + " CC 20 (127 then 0)");

        // turning the clock off sends Stop
        c.outWhat = TempoEngine::outBeats; c.midiOutClock = true; te.setConfig (0, c); e.tempoOut.update();
        for (int ms = 0; ms <= 2500; ++ms) e.tempoOut.tick (t1 + 4.0 + ms * 0.001);
        got.clear();
        c.midiOutClock = false; te.setConfig (0, c); e.tempoOut.update();
        check (got.size() == 1 && got[0].isMidiStop(), "send MIDI: clock off -> Stop");

        // OSC out to a real receiver on this Mac
        struct Collect final : juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
        {
            std::atomic<int> beats { 0 }, bpms { 0 };
            std::atomic<float> lastBpm { 0.0f };
            void oscMessageReceived (const juce::OSCMessage& m) override
            {
                if (m.getAddressPattern().toString() == "/lights/beat") ++beats;
                if (m.getAddressPattern().toString() == "/stagerack/bpm" && m.size() == 1 && m[0].isFloat32()) { ++bpms; lastBpm = m[0].getFloat32(); }
            }
        } collect;
        juce::OSCReceiver rx;
        const int port = 49000 + juce::Random::getSystemRandom().nextInt (2000);
        const bool listening = rx.connect (port);
        rx.addListener (&collect);

        e.tempoOut.midiSinkForTests = nullptr;
        c = te.getConfig (0);
        c.midiOutId = {}; c.midiOutKind = TempoEngine::midiNone; c.midiOutClock = false;
        c.oscOut = true; c.oscOutHost = "127.0.0.1"; c.oscOutPort = port; c.oscOutBeat = "/lights/beat"; c.oscOutBpm = {};
        te.setConfig (0, c);
        te.setBpm (0, 150.0);
        e.tempoOut.update();
        const auto t2 = TempoEngine::nowSec();
        for (int ms = 0; ms <= 2000; ++ms) e.tempoOut.tick (t2 + ms * 0.001);   // 2 s at 150 = 5 beats
        for (int w = 0; w < 100 && collect.beats.load() < 5; ++w) juce::Thread::sleep (10);
        check (listening && collect.beats.load() >= 4 && collect.beats.load() <= 6 && collect.bpms.load() >= 1 && std::abs (collect.lastBpm.load() - 150.0f) < 0.01f,
               "send OSC (UDP): 2 s at 150 -> " + juce::String (collect.beats.load()) + " x /lights/beat, /stagerack/bpm " + juce::String (collect.lastBpm.load(), 1));
        rx.removeListener (&collect);

        // OSC out to our own OSC-in port: refused (it would tap itself)
        e.osc.setEnabled (true, port + 1);
        c.oscOutPort = port + 1;
        te.setConfig (0, c);
        e.tempoOut.update();
        check (e.tempoOut.getProblem (0).contains ("loop"), "send OSC to our own OSC in: refused (" + e.tempoOut.getProblem (0) + ")");
        e.osc.setEnabled (false, port + 1);

        TempoEngine back;
        c.midiOutId = "IAC-id"; c.midiOutName = "IAC Bus 1"; c.midiOutKind = TempoEngine::midiNote; c.midiOutNumber = 61; c.outWhat = TempoEngine::outTaps;
        c.audioWindowPct = 7.0f;
        te.setConfig (0, c);
        back.fromTree (te.toTree());
        const auto b = back.getConfig (0);
        check (b.midiOutName == "IAC Bus 1" && b.midiOutNumber == 61 && b.outWhat == TempoEngine::outTaps && b.oscOut && b.oscOutBeat == "/lights/beat"
                 && b.oscOutPort == port + 1 && std::abs (b.audioWindowPct - 7.0f) < 1.0e-4f,
               "session keeps where the tempo is sent and the tolerance");
        c.oscOut = false; c.midiOutId = {};
        te.setConfig (0, c);
        e.tempoOut.update();
        check (! e.tempoOut.isRunning(), "nothing to send: the output thread stops");
    }
}

} // namespace stagerack
