#pragma once

#include "Engine/Session.h"
#include "Engine/MidiControl.h"

namespace stagerack
{

/*  --midi-loop-test: the whole CoreMIDI path with a virtual MIDImix in a child process
    (JUCE does not list a process's own virtual ports). The child (--midi-fake-midimix <log>)
    creates a source and a destination named "StageRack Test MIDI Mix", waits for the first LED
    (= StageRack found and mapped it), then plays a script and writes the steps and the LEDs it
    receives to the log. The parent runs a real AudioEngine MIDI input + Session + MidiControl and
    checks the racks and the LEDs at each step. */

static const char* const midiLoopName = "StageRack Test MIDI Mix";

inline void runOnMessageThread (std::function<void()> fn)
{
    juce::MessageManager::getInstance()->callFunctionOnMessageThread ([] (void* f) -> void* { (*static_cast<std::function<void()>*> (f))(); return nullptr; }, &fn);
}

inline void runFakeMidiMix (const juce::File& log)
{
    std::thread ([log]
    {
        static std::mutex lock;
        static juce::File logFile;
        logFile = log;
        auto write = [] (const juce::String& line)
        {
            const std::lock_guard<std::mutex> l (lock);
            logFile.appendText (line + "\n", false, false, "\n");
        };

        struct Leds final : juce::MidiInputCallback
        {
            std::function<void (const juce::String&)> write;
            std::atomic<int> count { 0 };
            void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
            {
                if (m.isNoteOnOrOff())
                {
                    ++count;
                    write ("led " + juce::String (m.getNoteNumber()) + " " + juce::String (m.isNoteOn() ? (int) m.getVelocity() : 0));
                }
            }
        };
        static Leds leds;
        leds.write = write;

        static std::unique_ptr<juce::MidiOutput> source;
        static std::unique_ptr<juce::MidiInput> dest;
        runOnMessageThread ([]
        {
            source = juce::MidiOutput::createNewDevice (midiLoopName);
            dest = juce::MidiInput::createNewDevice (midiLoopName, &leds);
            if (dest != nullptr) dest->start();
        });

        if (source == nullptr || dest == nullptr) { write ("error"); std::_Exit (1); }
        write ("ready");

        for (int i = 0; i < 300 && leds.count == 0; ++i) juce::Thread::sleep (50);   // StageRack connected
        juce::Thread::sleep (300);
        write ("connected");

        auto note = [] (int n) { source->sendMessageNow (juce::MidiMessage::noteOn (1, n, (juce::uint8) 127));
                                 source->sendMessageNow (juce::MidiMessage::noteOff (1, n)); };
        using namespace midiprofiles::midimix;

        for (int k = 0; k < 21; ++k)   // 21 presses: muted at the end
        {
            write ("sent " + juce::String (juce::Time::getHighResolutionTicks()));
            note (muteNote (0));
            juce::Thread::sleep (120);
        }
        juce::Thread::sleep (300);
        write ("step mute");

        const auto at = juce::roundToInt (midilaw::gainToNorm (0.0f) * 127.0f);
        for (int v : { at, at + 5, 127 })
            source->sendMessageNow (juce::MidiMessage::controllerEvent (1, faderCC (0), v));
        juce::Thread::sleep (500);
        write ("step fader");

        note (bankRightNote);
        juce::Thread::sleep (500);
        write ("step bank");
        juce::Thread::sleep (1200);   // the parent adds a 9th rack
        source->sendMessageNow (juce::MidiMessage::controllerEvent (1, 80, 127));   // unmapped: scene step
        juce::Thread::sleep (500);
        write ("end");
        std::_Exit (0);
    }).detach();
}

inline void runMidiLoopTest()
{
    int failures = 0;
    auto check = [&failures] (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
        if (! ok) ++failures;
    };

    const auto log = juce::File::createTempFile ("midiloop.txt");
    log.deleteFile();
    juce::ChildProcess child;
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
    check (child.start (juce::StringArray { exe, "--midi-fake-midimix", log.getFullPathName() }, 0), "fake MIDImix started (child process)");

    auto waitFor = [&log] (const juce::String& line, int ms)
    {
        for (int i = 0; i <= ms / 20; ++i)
        {
            if (juce::StringArray::fromLines (log.loadFileAsString()).contains (line)) return true;
            if (ms > 0) juce::Thread::sleep (20);
        }
        return false;
    };
    auto lastLed = [&log] (int n)
    {
        int v = -1;
        for (auto& l : juce::StringArray::fromLines (log.loadFileAsString()))
            if (l.startsWith ("led " + juce::String (n) + " ")) v = l.fromLastOccurrenceOf (" ", false, false).getIntValue();
        return v;
    };
    check (waitFor ("ready", 8000), "virtual controller created");

    AudioEngine* enginePtr = nullptr;   // leaked: the process ends with _Exit
    Session* sessionPtr = nullptr;
    MidiControl* midiPtr = nullptr;
    static std::atomic<int> sceneSteps { 0 };
    runOnMessageThread ([&]
    {
        enginePtr = new AudioEngine();
        enginePtr->listenToMidiInputs();
        sessionPtr = new Session (*enginePtr, *new PluginManager());
        midiPtr = new MidiControl (*sessionPtr);
        enginePtr->midiRemote = [m = midiPtr] (const juce::String& i, const juce::String& n, const juce::MidiMessage& msg) { return m->handleMidi (i, n, msg); };
        enginePtr->onSceneStep = [] (int) { ++sceneSteps; };
        sessionPtr->newSession();
        for (int i = 1; i < 8; ++i) sessionPtr->addRack();
    });
    auto& engine = *enginePtr;
    auto& session = *sessionPtr;
    auto& midi = *midiPtr;

    juce::String id;
    for (int attempt = 0; attempt < 40 && id.isEmpty(); ++attempt)
    {
        juce::Array<juce::MidiDeviceInfo> ins;
        runOnMessageThread ([&] { ins = juce::MidiInput::getAvailableDevices(); });
        for (auto& d : ins)
            if (d.name == midiLoopName) id = d.identifier;
        if (id.isEmpty()) juce::Thread::sleep (100);
    }
    check (id.isNotEmpty(), "the controller is listed as a MIDI input");

    int mapped = 0;
    runOnMessageThread ([&] { midi.openDevices(); mapped = midi.quickSetup (id); });   // known name ("MIDI Mix"): quick setup
    check (mapped > 50, "quick setup on the MIDImix (" + juce::String (mapped) + " mappings)");

    auto& r1 = engine.getRack (0);
    check (waitFor ("connected", 8000), "LEDs reach the controller (output found by name)");

    // latency: from the controller's send (same mach clock in both processes) to the mute in the rack
    std::vector<juce::int64> flips;
    {
        bool state = r1.muted.load();
        const auto start = juce::Time::getHighResolutionTicks();
        auto secondsSince = [] (juce::int64 t) { return juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t); };
        while (secondsSince (start) < 8.0 && (flips.empty() || secondsSince (flips.back()) < 0.6))
            if (r1.muted.load() != state) { state = ! state; flips.push_back (juce::Time::getHighResolutionTicks()); }
    }
    waitFor ("step mute", 3000);
    std::vector<juce::int64> sent;
    for (auto& l : juce::StringArray::fromLines (log.loadFileAsString()))
        if (l.startsWith ("sent ")) sent.push_back (l.fromFirstOccurrenceOf (" ", false, false).getLargeIntValue());
    std::vector<double> lat;
    for (auto f : flips)   // each flip against the last press sent before it
    {
        juce::int64 before = 0;
        for (auto t : sent) if (t <= f) before = t;
        if (before != 0) lat.push_back (juce::Time::highResolutionTicksToSeconds (f - before) * 1000.0);
    }
    std::sort (lat.begin(), lat.end());
    const auto median = lat.empty() ? -1.0 : lat[lat.size() / 2], worst = lat.empty() ? -1.0 : lat.back();
    check (lat.size() >= 19 && median >= 0.0 && median < 5.0 && worst < 40.0,
           "button to mute, " + juce::String ((int) lat.size()) + " presses: median " + juce::String (median, 2) + " ms, max " + juce::String (worst, 2) + " ms");
    check (r1.muted.load(), "Mute 1 through CoreMIDI mutes rack 1");
    check (lastLed (midiprofiles::midimix::muteNote (0)) == 127, "Mute 1 LED lit on the controller");

    waitFor ("step fader", 3000);
    check (std::abs (r1.gainDb.load() - 12.0f) < 0.01f, "Fader 1 picks up and drives the gain (+12 dB at the top)");

    waitFor ("step bank", 3000);
    check (midi.getBankOffset() == 0 && lastLed (midiprofiles::midimix::bankRightNote) == 0, "8 racks: Bank Right does nothing, its LED is off");
    runOnMessageThread ([&] { session.addRack(); });
    waitFor ("end", 4000);
    check (lastLed (midiprofiles::midimix::bankRightNote) == 127, "a 9th rack: the Bank Right LED lights");
    check (sceneSteps == 1, "unmapped CC 80 still steps the scene");

    std::cout << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED") << std::endl;
    log.deleteFile();
    std::fflush (nullptr);
    std::_Exit (failures == 0 ? 0 : 1);
}

} // namespace stagerack
