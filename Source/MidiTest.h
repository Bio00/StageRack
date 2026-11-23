#pragma once

#include "Engine/Session.h"
#include "Engine/MidiControl.h"
#include "Plugins/ParamTwins.h"

namespace stagerack
{

/** A plugin with the parameters of a tempo delay (like Arturia TAPE-201): free and synced rates + switches. */
struct TwinTestPlugin final : juce::AudioPluginInstance
{
    TwinTestPlugin()
    {
        juce::StringArray divisions;
        for (int i = 0; i < 19; ++i) divisions.add ("1/" + juce::String (i + 1));
        juce::AudioProcessor::addParameter (new juce::AudioParameterFloat (juce::ParameterID { "rl", 1 }, "Repeat Rate Left", 0.0f, 1.0f, 0.5f));
        juce::AudioProcessor::addParameter (new juce::AudioParameterChoice (juce::ParameterID { "rls", 1 }, "Repeat Rate Left Sync", divisions, 9));
        juce::AudioProcessor::addParameter (new juce::AudioParameterBool (juce::ParameterID { "rs", 1 }, "Rates Sync", false));
        juce::AudioProcessor::addParameter (new juce::AudioParameterFloat (juce::ParameterID { "lr", 1 }, "LFO Rate", 0.0f, 1.0f, 0.5f));
        juce::AudioProcessor::addParameter (new juce::AudioParameterChoice (juce::ParameterID { "lrs", 1 }, "LFO Rate Sync", divisions, 9));
        juce::AudioProcessor::addParameter (new juce::AudioParameterBool (juce::ParameterID { "ls", 1 }, "LFO Sync", false));
    }
    const juce::String getName() const override                        { return "Twin Test"; }
    void prepareToPlay (double, int) override                            {}
    void releaseResources() override                                     {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override                         { return 0.0; }
    bool acceptsMidi() const override                                    { return false; }
    bool producesMidi() const override                                   { return false; }
    bool hasEditor() const override                                      { return false; }
    juce::AudioProcessorEditor* createEditor() override                  { return nullptr; }
    int getNumPrograms() override                                        { return 1; }
    int getCurrentProgram() override                                     { return 0; }
    void setCurrentProgram (int) override                                {}
    const juce::String getProgramName (int) override                     { return {}; }
    void changeProgramName (int, const juce::String&) override          {}
    void getStateInformation (juce::MemoryBlock&) override               {}
    void setStateInformation (const void*, int) override                 {}
    void fillInPluginDescription (juce::PluginDescription& d) const override { d.name = getName(); d.pluginFormatName = "Test"; }
};

/** --selftest: MIDI remote (mappings, pick-up, LEDs, shift layer, banks, learn, session). */
inline void runMidiSelfTest (const std::function<void (bool, const juce::String&)>& check)
{
    AudioEngine e;
    auto* pm = new PluginManager();   // leaked on purpose (its destructor saves files)
    Session s (e, *pm);
    MidiControl midi (s);
    s.newSession();

    for (int i = 1; i < 10; ++i)
        s.addRack();   // 10 racks

    const juce::String dev = "test-midimix", devName = "MIDI Mix";
    midi.setControllerEnabled (dev, devName, true);

    std::vector<juce::MidiMessage> sent;
    midi.feedbackSinkForTests = [&] (const juce::String& id, const juce::MidiMessage& m) { if (id == dev) sent.push_back (m); };
    auto ledOf = [&] (int note) -> int
    {
        int v = -1;
        for (auto& m : sent)
            if (m.isNoteOnOrOff() && m.getNoteNumber() == note)
                v = m.isNoteOn() ? m.getVelocity() : 0;
        return v;
    };
    auto cc = [&] (int n, int v) { midi.injectForTests (dev, juce::MidiMessage::controllerEvent (1, n, v)); };
    auto noteOn = [&] (int n) { midi.injectForTests (dev, juce::MidiMessage::noteOn (1, n, (juce::uint8) 127)); };
    auto noteOff = [&] (int n) { midi.injectForTests (dev, juce::MidiMessage::noteOff (1, n)); };
    auto press = [&] (int n) { noteOn (n); noteOff (n); };
    auto& r1 = e.getRack (0);
    auto& r9 = e.getRack (8);

    using namespace midiprofiles::midimix;
    const auto n = midi.quickSetup (dev);
    for (auto m : midi.getMappings())   // the default is now "jump": test the pick-up explicitly
    {
        m.takeover = MidiMapping::pickup;
        midi.updateMapping (m);
    }
    check (n == 8 * 7 + 3, "MIDImix quick setup: 8 strips x 7 controls + bank < > + SOLO (" + juce::String (n) + ")");
    check (midi.describeSource (midi.getMappings().front()) == "Fader 1", "MIDImix controls have their names (Fader 1)");

    // pick-up: the fader at the top does not make the rack jump from 0 dB to +12
    r1.gainDb = 0.0f;
    cc (faderCC (0), 127);
    check (std::abs (r1.gainDb.load()) < 0.01f, "pick-up: a fader far from the value does nothing");
    const auto at0dB = juce::roundToInt (midilaw::gainToNorm (0.0f) * 127.0f);
    cc (faderCC (0), at0dB + 6);
    cc (faderCC (0), at0dB - 1);   // crosses the value: caught
    cc (faderCC (0), 127);
    check (std::abs (r1.gainDb.load() - 12.0f) < 0.01f, "pick-up: once the fader crossed the value it drives it (+12 dB at the top)");
    cc (faderCC (0), 0);
    check (r1.gainDb.load() <= -59.9f, "fader at the bottom = -60 dB");

    // mute: toggle on press, release ignored, LED follows
    sent.clear();
    press (muteNote (0));
    midi.runFeedbackForTests();
    check (r1.muted.load() && ledOf (muteNote (0)) == 127, "Mute 1 toggles the mute of rack 1, its LED lights");
    press (muteNote (0));
    midi.runFeedbackForTests();
    check (! r1.muted.load() && ledOf (muteNote (0)) == 0, "Mute 1 again: unmuted, LED off");

    // a mute changed with the mouse: the LED follows it too
    r1.muted = true;
    sent.clear();
    midi.runFeedbackForTests();
    check (ledOf (muteNote (0)) == 127, "LED follows a mute made elsewhere (mouse, scene)");
    r1.muted = false;

    // SOLO held: the Mute LEDs show the solos, the Mute buttons send the solo notes
    noteOn (soloButtonNote);
    sent.clear();
    midi.runFeedbackForTests();
    check (midi.isShiftHeld() && ledOf (muteNote (0)) == 0, "SOLO held: shift layer, Mute LED shows the solo (off)");
    press (soloNote (0));
    midi.runFeedbackForTests();
    check (r1.solo.load() && ! r1.muted.load() && ledOf (muteNote (0)) == 127, "SOLO + Mute 1 = solo rack 1, LED on");
    noteOff (soloButtonNote);
    midi.runFeedbackForTests();
    check (! midi.isShiftHeld() && ledOf (muteNote (0)) == 0, "SOLO released: the Mute LED shows the mute again");
    r1.solo = false;

    // banks: Bank Right moves the 8 strips to racks 9-10
    sent.clear();
    midi.runFeedbackForTests();
    press (bankRightNote);
    midi.runFeedbackForTests();
    check (midi.getBankOffset() == 8 && ledOf (bankLeftNote) == 127 && ledOf (bankRightNote) == 0,
           "Bank Right: racks 9-16, Bank Left LED on, Bank Right off (no more racks)");
    press (muteNote (0));
    check (r9.muted.load() && ! r1.muted.load(), "after Bank Right, Mute 1 mutes rack 9");
    r9.gainDb = -10.0f;
    cc (faderCC (0), 0);
    check (std::abs (r9.gainDb.load() + 10.0f) < 0.01f, "new bank: the fader has to pick the new rack's value up");
    press (muteNote (2));   // strip 3 of this bank: no rack there
    press (bankRightNote);
    check (midi.getBankOffset() == 8, "Bank Right at the last bank: stays");
    press (bankLeftNote);
    check (midi.getBankOffset() == 0, "Bank Left: back to racks 1-8");

    // knobs: input trim and sends (absolute, pick-up)
    r1.inputGainDb = -24.0f;
    cc (knobCC (0, 0), 0);
    cc (knobCC (0, 0), 127);
    check (std::abs (r1.inputGainDb.load() - 24.0f) < 0.01f, "Knob 1A = input trim of rack 1 (+24 dB at the end)");

    // map mode learn: select a target, move a control
    midi.setMapMode (true);
    midi.setLearnTarget (MidiTarget::forRack (MidiTarget::rackWeight, MidiTarget::fixedRack, e.getRack (3).getId()));
    cc (masterCC, 40);
    cc (masterCC, 41);   // still the same knob: one mapping
    const auto weightMaps = midi.mappingsFor (midi.getLearnTarget());
    check (weightMaps.size() == 1 && weightMaps[0]->source.number == masterCC, "map mode: moving a control maps it to the selected target");
    cc (7, 10);           // another control while the same target is selected: the mapping moves to it
    const auto moved = midi.mappingsFor (midi.getLearnTarget());
    check (moved.size() == 1 && moved[0]->source.number == 7, "map mode: another control moves the mapping (Ableton)");
    check (std::abs (e.getRack (3).automixWeightDb.load()) < 0.01f, "map mode: learning does not change the value");
    midi.setMapMode (false);

    // range and inversion, no takeover
    {
        auto m = *moved[0];
        m.minValue = midilaw::weightToNorm (6.0f);
        m.maxValue = midilaw::weightToNorm (-20.0f);   // inverted
        m.takeover = MidiMapping::takeoverNone;
        midi.updateMapping (m);
        cc (7, 0);
        check (std::abs (e.getRack (3).automixWeightDb.load() - 6.0f) < 0.05f, "inverted range: control at 0 = max (+6 dB)");
        cc (7, 127);
        check (std::abs (e.getRack (3).automixWeightDb.load() + 20.0f) < 0.05f, "inverted range: control at the top = min (-20 dB)");
    }

    // relative encoder
    {
        MidiMapping m;
        m.device = dev; m.deviceName = devName;
        m.source = { MidiSource::cc, 2, 20 };
        m.target = MidiTarget::forRack (MidiTarget::rackGain, MidiTarget::fixedRack, e.getRack (5).getId());
        m.mode = MidiMapping::relTwos;
        midi.addMapping (m);
        e.getRack (5).gainDb = 0.0f;
        const auto before = midilaw::gainToNorm (0.0f);
        midi.injectForTests (dev, juce::MidiMessage::controllerEvent (2, 20, 5));     // +5
        midi.injectForTests (dev, juce::MidiMessage::controllerEvent (2, 20, 126));   // -2
        check (std::abs (midilaw::gainToNorm (e.getRack (5).gainDb.load()) - (before + 0.03f)) < 0.002f, "relative encoder (2's complement): +5 -2 steps");
    }

    // a plugin parameter with a tempo-synced twin (TAPE-201: "Repeat Rate Left" / "... Sync" / "Rates Sync")
    {
        auto& r7 = e.getRack (6);
        r7.setPlugin (0, std::make_unique<TwinTestPlugin>());
        auto* p = r7.getPlugin (0);
        auto& prm = p->getParameters();
        check (paramtwins::active (prm, prm[0]) == prm[0] && paramtwins::active (prm, prm[3]) == prm[3], "twins: sync off, the free rate is in use");
        prm[2]->setValueNotifyingHost (1.0f);   // Rates Sync on
        check (paramtwins::active (prm, prm[0]) == prm[1] && paramtwins::active (prm, prm[3]) == prm[3],
               "twins: Rates Sync on = Repeat Rate Left Sync in use; LFO Rate follows its own switch (LFO Sync)");

        MidiMapping m;
        m.device = dev; m.source = { MidiSource::cc, 3, 21 };
        m.takeover = MidiMapping::takeoverNone;
        m.target = MidiTarget::forRack (MidiTarget::pluginParam, MidiTarget::fixedRack, r7.getId(), 0, 1);   // learned from the synced knob
        m.target.paramName = "Repeat Rate Left Sync";
        const auto id = midi.addMapping (m);
        check (midi.findMapping (id) != nullptr && midi.findMapping (id)->target.param == 0, "a synced twin is stored as one control (the free rate)");

        prm[0]->setValueNotifyingHost (0.5f);
        midi.injectForTests (dev, juce::MidiMessage::controllerEvent (3, 21, 127));
        check (prm[1]->getValue() > 0.99f && std::abs (prm[0]->getValue() - 0.5f) < 0.001f, "plugin in sync: the control moves the synced rate (1/8, 1/4.t...)");
        check (midi.describeTarget (midi.findMapping (id)->target).contains ("now Repeat Rate Left Sync"), "the MIDI window says which twin is in use");

        prm[2]->setValueNotifyingHost (0.0f);   // sync off
        midi.injectForTests (dev, juce::MidiMessage::controllerEvent (3, 21, 0));
        check (prm[0]->getValue() < 0.01f && prm[1]->getValue() > 0.99f, "sync off: the same control moves the free rate again");
        midi.removeMapping (id);
    }

    // scene trigger, tap, clear solos
    s.addScene ("A");
    s.addScene ("B");
    {
        MidiMapping m;
        m.device = dev; m.source = { MidiSource::note, 1, 60 };
        m.target = MidiTarget::global (MidiTarget::sceneRecall, 0);
        midi.addMapping (m);
        press (60);
        check (s.getCurrentScene() == 0, "a button recalls scene 1");
        sent.clear();
        midi.runFeedbackForTests();
        check (ledOf (60) == 127, "scene button LED lit while its scene is the current one");
    }

    // session: saved with the file, loaded back
    const auto mapsBefore = midi.getMappings().size();
    auto tree = s.toTree();
    s.newSession();
    check (midi.getMappings().empty(), "new session: no mappings (no default map)");
    s.loadFromTree (tree, {}, false);
    check (midi.getMappings().size() == mapsBefore, "mappings saved in the session and loaded back ("
                                                   + juce::String ((int) midi.getMappings().size()) + ")");

    // default map for sessions saved without one
    midi.defaultMap = midi.toTree();
    s.newSession();
    check (midi.getMappings().size() == mapsBefore, "new session starts from the default map");
    midi.defaultMap = {};

    // the engine hands mapped messages to the remote, the rest to scenes / tempo
    int scenesStepped = 0;
    e.onSceneStep = [&] (int) { ++scenesStepped; };
    check (midi.handleMidi (dev, devName, juce::MidiMessage::noteOn (1, muteNote (0), (juce::uint8) 127)),
           "a mapped message is consumed (scenes / tempo do not see it)");
    check (! midi.handleMidi (dev, devName, juce::MidiMessage::controllerEvent (1, 80, 127)), "an unmapped message is left to the scene CCs");
    check (! midi.handleMidi ("other", "Keyboard", juce::MidiMessage::noteOn (1, muteNote (0), (juce::uint8) 127)),
           "another device's message is not taken");

    midi.feedbackSinkForTests = nullptr;
    s.newSession();
}

} // namespace stagerack
