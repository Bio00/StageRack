#pragma once

#include "Engine/Session.h"

namespace stagerack
{

/** --selftest: buses and sends (Ableton-style returns, sub-groups, master bus). */
inline void runRoutingSelfTest (const std::function<void (bool, const juce::String&)>& check)
{
    const int bs = 64;
    const double sr = 48000.0;
    juce::AudioBuffer<float> in (2, bs), out (6, bs);
    juce::AudioIODeviceCallbackContext ctx;

    auto runBlocks = [&] (AudioEngine& e, int n, float value, bool impulse = false)
    {
        for (int k = 0; k < n; ++k)
        {
            in.clear();
            if (impulse) in.setSample (0, 0, 1.0f);
            else         for (int i = 0; i < bs; ++i) in.setSample (0, i, value);
            e.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 6, bs, ctx);
        }
    };
    auto near = [] (float a, float b) { return std::abs (a - b) < 0.01f; };
    auto db = [] (float d) { return juce::Decibels::decibelsToGain (d); };

    {
        AudioEngine e;
        // the bus is first in the list: the stages must still run it after the tracks
        auto& bus = e.addRack();
        auto& t1 = e.addRack();
        auto& t2 = e.addRack();
        bus.inputChannel = 0; bus.outputChannel = 2;              // its own input only counts while routing is off
        t1.inputChannel = 0; t1.stereoInput = false; t1.outputChannel = 0;
        t2.inputChannel = 0; t2.stereoInput = false; t2.outputChannel = 4;
        check (e.setRackBus (bus, 1) && ! e.setRackBus (t2, 1), "bus letters are unique");
        t1.sends[0].db = -6.0f;
        e.prepare (sr, bs, {});

        runBlocks (e, 50, 0.5f);
        check (near (out.getSample (2, bs - 1), 0.5f) && near (out.getSample (0, bs - 1), 0.5f),
               "routing off: the bus rack plays its own input, sends ignored");

        e.setRoutingEnabled (true);
        check (e.findBus (1) == &bus && bus.stage > t1.stage, "routing on: Bus A runs in a later stage than the tracks");
        runBlocks (e, 50, 0.5f);
        check (near (out.getSample (2, bs - 1), 0.5f * db (-6.0f)) && near (out.getSample (3, bs - 1), 0.5f * db (-6.0f))
                 && near (out.getSample (0, bs - 1), 0.5f),
               "send -6 dB into Bus A (" + juce::String (out.getSample (2, bs - 1), 3) + "), dry track unchanged");

        runBlocks (e, 1, 0.0f, true);
        check (out.getSample (2, 0) > 0.3f, "send: zero latency (impulse on the bus output in the same callback)");

        // post-fader follows the fader, pre-fader does not; mute silences both
        t1.gainDb = -20.0f;
        runBlocks (e, 100, 0.5f);
        check (near (out.getSample (2, bs - 1), 0.5f * db (-26.0f)), "post-fader send follows the track fader");
        t1.sends[0].pre = true;
        runBlocks (e, 20, 0.5f);
        check (near (out.getSample (2, bs - 1), 0.5f * db (-6.0f)), "pre-fader send ignores the fader");
        t1.muted = true;
        runBlocks (e, 20, 0.5f);
        check (std::abs (out.getSample (2, bs - 1)) < 1.0e-5f, "muted track: pre-fader send silent too");
        t1.muted = false; t1.gainDb = 0.0f; t1.sends[0].pre = false;
        runBlocks (e, 100, 0.5f);

        // sub-group: T2 routed into Bus B, Bus B -> outputs 1-2 together with T1
        auto& busB = e.addRack();
        busB.inputChannel = -1; busB.outputChannel = 0;
        check (e.setRackBus (busB, 2) && e.setRackOutputBus (t2, 2), "T2 routed into Bus B");
        busB.gainDb = -6.0f;
        runBlocks (e, 100, 0.5f);
        check (std::abs (out.getSample (4, bs - 1)) < 1.0e-6f && near (out.getSample (0, bs - 1), 0.5f + 0.5f * db (-6.0f)),
               "routed track leaves its device output; Bus B adds it at -6 dB (" + juce::String (out.getSample (0, bs - 1), 3) + ")");

        // bus -> bus (returns into a master bus) and loops
        check (e.setRackOutputBus (bus, 2), "Bus A routed into Bus B");
        check (! e.setRackOutputBus (busB, 1) && ! e.canRouteToBus (busB, 1) && ! e.canSendTo (busB, 1),
               "Bus B -> Bus A refused (loop), its send to A disabled");
        check (! e.canRouteToBus (bus, 1) && e.canSendTo (t1, 2), "no bus into itself; tracks can send anywhere");
        runBlocks (e, 100, 0.5f);
        check (near (out.getSample (0, bs - 1), 0.5f + db (-6.0f) * (0.5f + 0.5f * db (-6.0f))) && std::abs (out.getSample (2, bs - 1)) < 1.0e-6f,
               "Bus A -> Bus B -> OUT 1-2 (" + juce::String (out.getSample (0, bs - 1), 3) + ")");

        // solo T1: its send keeps Bus A (and B after it) audible, T2 into Bus B is gone
        t1.solo = true;
        runBlocks (e, 100, 0.5f);
        check (near (out.getSample (0, bs - 1), 0.5f + db (-6.0f) * 0.5f * db (-6.0f)),
               "solo track: heard with its return chain, other track muted (" + juce::String (out.getSample (0, bs - 1), 3) + ")");
        t1.solo = false;
        busB.solo = true;
        runBlocks (e, 100, 0.5f);
        check (near (out.getSample (0, bs - 1), db (-6.0f) * (0.5f + 0.5f * db (-6.0f))),
               "solo bus: hears what feeds it, the dry tracks are muted (" + juce::String (out.getSample (0, bs - 1), 3) + ")");
        busB.solo = false;

        // a missing bus: the routed rack goes silent (never the device by surprise)
        e.removeRack (busB);
        runBlocks (e, 20, 0.5f);
        check (std::abs (out.getSample (4, bs - 1)) < 1.0e-6f && near (out.getSample (0, bs - 1), 0.5f),
               "bus deleted: racks routed into it are silent");

        e.setRoutingEnabled (false);
        runBlocks (e, 50, 0.5f);
        check (near (out.getSample (4, bs - 1), 0.5f) && near (out.getSample (2, bs - 1), 0.5f),
               "routing off again: every rack back on its own I/O");
        e.audioDeviceStopped();
    }

    // buses send to each other in any direction while those sends are off; turning one on
    // closes the opposite direction (a loop), turning it off opens it again
    {
        AudioEngine e;
        e.setRoutingEnabled (true);
        auto& a = e.addRack();
        auto& b = e.addRack();
        auto& c = e.addRack();
        a.inputChannel = b.inputChannel = c.inputChannel = -1;
        a.outputChannel = b.outputChannel = 0;
        c.outputChannel = 4;
        e.setRackBus (a, 1);
        e.setRackBus (b, 2);
        e.setRackBus (c, 3);
        check (e.canSendTo (c, 1) && e.canSendTo (c, 2) && e.canSendTo (a, 3) && e.canSendTo (b, 1),
               "a bus created later can send to the earlier ones (and back) while the sends are off");

        e.setSend (c, 1, -6.0f);
        check (! e.canSendTo (a, 3) && e.canSendTo (b, 3) && a.stage > c.stage,
               "Bus C -> Bus A on: A -> C closed (loop), A runs after C");

        e.prepare (sr, bs, {});
        auto& t = e.addRack();
        t.inputChannel = 0; t.stereoInput = false; t.outputChannel = -1;
        e.setSend (t, 3, 0.0f);
        runBlocks (e, 50, 0.5f);
        check (near (out.getSample (0, bs - 1), 0.5f * db (-6.0f)) && near (out.getSample (4, bs - 1), 0.5f),
               "track -> Bus C -> send -6 dB -> Bus A (" + juce::String (out.getSample (0, bs - 1), 3) + ")");

        e.setSend (c, 1, Rack::sendOffDb);
        e.rebuildRouting();   // the app does this ~150 ms later, once the send has faded out
        check (e.canSendTo (a, 3), "Bus C -> Bus A off again: A -> C possible again");
        e.audioDeviceStopped();
    }

    // session: saved with the racks, sends recalled by scenes, a duplicated bus gets a new letter
    {
        AudioEngine e;
        auto* pm = new PluginManager();   // leaked on purpose (its destructor saves files)
        Session s (e, *pm);
        s.newSession();
        auto* bus = s.addBus();
        check (bus != nullptr && e.isRoutingEnabled() && bus->busIndex == 1 && bus->inputChannel == -1 && bus->name == "Bus A",
               "Add Bus: routing on, Bus A, no device input");

        auto& t = e.getRack (0);
        t.sends[0].db = -3.0f;
        t.sends[0].pre = true;
        s.addScene ("Loud");
        t.sends[0].db = -20.0f;
        s.addScene ("Soft");
        s.recallScene (0);
        check (std::abs (t.sends[0].db.load() + 3.0f) < 1.0e-3f, "scene recalls the send level");

        s.duplicateRacks ({ bus->getId() });
        check (e.getNumRacks() == 3 && e.getRack (2).busIndex == 2 && e.findBus (2) == &e.getRack (2), "duplicated bus becomes Bus B");
        check (e.setRackOutputBus (t, 2), "track routed into Bus B");

        const auto tree = s.toTree();
        AudioEngine e2;
        Session s2 (e2, *pm);
        s2.loadFromTree (tree, {}, false);
        auto& t2 = e2.getRack (0);
        check (e2.isRoutingEnabled() && e2.getRack (1).busIndex == 1 && t2.outputBus == 2 && t2.sends[0].pre.load()
                 && std::abs (t2.sends[0].db.load() + 3.0f) < 1.0e-3f && t2.outBusRack == &e2.getRack (2),
               "session keeps routing on/off, buses, routed outputs and sends");

        s2.setRoutingEnabled (false);
        check (! e2.isRoutingEnabled() && t2.outBusRack == nullptr && s2.undo() && e2.isRoutingEnabled() && t2.outBusRack != nullptr,
               "routing on/off is undoable");
    }

    // multiple outputs (Pro Tools style): extra device pairs / mono channels / buses
    {
        AudioEngine e;
        auto& t = e.addRack();
        auto& bus = e.addRack();
        t.inputChannel = 0; t.stereoInput = false; t.outputChannel = 0;
        bus.inputChannel = -1; bus.outputChannel = 4;
        e.prepare (sr, bs, {});

        const auto kept = e.setRackExtraOutputs (t, { Rack::outStereo + 2, Rack::outStereo + 0, Rack::outBus + 1, Rack::outStereo + 2 });
        check (kept == std::vector<int> ({ Rack::outStereo + 2, Rack::outBus + 1 }), "extra outputs: the main one and duplicates dropped");

        runBlocks (e, 50, 0.5f);
        check (near (out.getSample (0, bs - 1), 0.5f) && near (out.getSample (3, bs - 1), 0.5f) && std::abs (out.getSample (4, bs - 1)) < 1.0e-6f,
               "routing off: OUT 1-2 and OUT 3-4, the extra bus ignored");

        check (e.setRackBus (bus, 1), "Bus A");
        e.setRoutingEnabled (true);
        check (bus.stage > t.stage && t.feedsABus, "routing on: the extra output feeds Bus A (later stage)");
        runBlocks (e, 50, 0.5f);
        check (near (out.getSample (0, bs - 1), 0.5f) && near (out.getSample (2, bs - 1), 0.5f) && near (out.getSample (4, bs - 1), 0.5f),
               "routing on: OUT 1-2, OUT 3-4 and Bus A (OUT 5) all play it");
        runBlocks (e, 1, 0.0f, true);
        check (out.getSample (4, 0) > 0.3f && out.getSample (2, 0) > 0.3f, "extra outputs: zero latency");

        t.gainDb = -12.0f;
        runBlocks (e, 100, 0.5f);
        check (near (out.getSample (2, bs - 1), 0.5f * db (-12.0f)) && near (out.getSample (4, bs - 1), 0.5f * db (-12.0f)),
               "every destination is after the fader");
        t.gainDb = 0.0f;

        e.setRackExtraOutputs (t, { Rack::outMono + 5 });
        runBlocks (e, 100, 0.5f);
        check (near (out.getSample (5, bs - 1), 0.5f) && std::abs (out.getSample (2, bs - 1)) < 1.0e-6f && std::abs (out.getSample (4, bs - 1)) < 1.0e-6f,
               "extra mono output (L+R)/2 on OUT 6; the old extras are gone");

        // main into the bus, extra pair to the device; a bus never outputs into itself
        check (e.setRackOutputBus (t, 1), "main output into Bus A");
        e.setRackExtraOutputs (t, { Rack::outStereo + 2 });
        runBlocks (e, 100, 0.5f);
        check (std::abs (out.getSample (0, bs - 1)) < 1.0e-6f && near (out.getSample (2, bs - 1), 0.5f) && near (out.getSample (4, bs - 1), 0.5f),
               "main into Bus A + extra OUT 3-4");
        check (e.setRackExtraOutputs (bus, { Rack::outBus + 1, Rack::outStereo + 0 }) == std::vector<int> ({ Rack::outStereo + 0 }),
               "a bus cannot output into itself");

        // solo another rack: this one is silent on every destination
        auto& other = e.addRack();
        other.inputChannel = -1; other.outputChannel = -1;
        e.prepare (sr, bs, {});
        other.solo = true;
        runBlocks (e, 100, 0.5f);
        check (std::abs (out.getSample (2, bs - 1)) < 1.0e-6f && std::abs (out.getSample (4, bs - 1)) < 1.0e-6f, "solo elsewhere: extra outputs silent too");
        e.audioDeviceStopped();
    }

    // extra outputs are saved with the session and undoable
    {
        AudioEngine e;
        auto* pm = new PluginManager();
        Session s (e, *pm);
        s.newSession();
        auto* bus = s.addBus();
        auto& t = e.getRack (0);
        e.setRackExtraOutputs (t, { Rack::outStereo + 4, Rack::outBus + 1 });
        s.markDirty();

        AudioEngine e2;
        Session s2 (e2, *pm);
        s2.loadFromTree (s.toTree(), {}, false);
        check (bus != nullptr && e2.getRack (0).getExtraOutputs() == std::vector<int> ({ Rack::outStereo + 4, Rack::outBus + 1 }) && e2.getRack (0).feedsABus,
               "session keeps the extra outputs (and they feed the bus again)");

        e.setRackExtraOutputs (t, {});
        s.markDirty();
        check (s.undo() && t.getExtraOutputs().size() == 2 && t.feedsABus, "extra outputs: undo");

        // view (Pro Tools style): saved with the session; a rack can choose its own sends
        Session::RackView v;
        v.info = false; v.sendsMask = 0b10;
        s.setRackView (v);
        s.setRackSendsShown (t, 0b1);
        check (s.sendsShown (t) == 0b1 && s.sendsShown (*bus) == 0b10, "sends shown: the rack's own choice, else the view's");
        AudioEngine e3;
        Session s3 (e3, *pm);
        s3.loadFromTree (s.toTree(), {}, false);
        check (s3.getRackView() == v && e3.getRack (0).uiSendsShown == 0b1 && s3.sendsShown (e3.getRack (1)) == 0b10,
               "view and per-rack sends saved with the session");
        v.sends = false;
        s3.setRackView (v);
        check (s3.sendsShown (e3.getRack (0)) == 0, "view without sends: none shown on any rack");

        // one rack with its own sections: only its sends, while the others follow the view
        s3.setRackHiddenSections (e3.getRack (1), 1 | 2 | 4 | 16);
        const auto own = s3.viewFor (e3.getRack (1)), others = s3.viewFor (e3.getRack (0));
        check (own.sends && ! own.inserts && ! own.io && ! own.input && ! own.info && s3.sendsShown (e3.getRack (1)) == 0b10
                 && ! others.sends && others.inserts && others.io,
               "a rack with only its sends, the others as the view (no sends)");
        AudioEngine e4;
        Session s4 (e4, *pm);
        s4.loadFromTree (s3.toTree(), {}, false);
        check (e4.getRack (1).uiHideSections == (1 | 2 | 4 | 16) && e4.getRack (0).uiHideSections == -1, "a rack's own sections are saved");
        s4.setRackHiddenSections (e4.getRack (1), -1);
        check (s4.viewFor (e4.getRack (1)).sends == false, "back to the view");
        s3.newSession();
        check (s3.getRackView() == Session::RackView(), "new session: everything shown");
    }

    // inf / NaN from the device: the block is silenced (meters, buses, outputs stay finite)
    {
        AudioEngine e;
        e.setRoutingEnabled (true);
        auto& t = e.addRack();
        auto& bus = e.addRack();
        t.inputChannel = 0; t.stereoInput = false; t.outputChannel = 0;
        bus.inputChannel = -1; bus.outputChannel = 2;
        e.setRackBus (bus, 1);
        e.setSend (t, 1, 0.0f);
        e.prepare (sr, bs, {});
        runBlocks (e, 5, std::numeric_limits<float>::infinity());
        const auto inPeak = t.inMeter[0].readAndReset(), outPeak = bus.outMeter[0].readAndReset();
        bool finiteOut = true;
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < bs; ++i)
                finiteOut = finiteOut && std::isfinite (out.getSample (ch, i)) && out.getSample (ch, i) == 0.0f;
        check (finiteOut && std::isfinite (inPeak) && std::isfinite (outPeak) && t.nonFiniteBlocks.load() >= 5,
               "infinite input: silenced block, meters and bus stay finite");
        runBlocks (e, 50, 0.5f);
        check (near (out.getSample (0, bs - 1), 0.5f) && near (out.getSample (2, bs - 1), 0.5f), "normal audio again right after");
        e.audioDeviceStopped();
    }

    // folders (Ableton group tracks): members right after the folder, routed into it
    {
        AudioEngine e;
        auto* pm = new PluginManager();
        Session s (e, *pm);
        s.newSession();
        s.addRacks (4, false);   // 5 racks: 0..4
        const auto id = [&] (int i) { return e.getRack (i).getId(); };
        const int r1 = id (1), r3 = id (3), r4 = id (4);
        e.getRack (3).uiSendsV = 120.0f;
        e.getRack (3).uiInputV = 0.0f;

        auto* f = s.groupIntoFolder ({ r1, r3 });
        check (f != nullptr && f->isFolder && e.isRoutingEnabled() && e.indexOf (*f) == 1
                 && e.getRack (2).getId() == r1 && e.getRack (3).getId() == r3 && e.getRack (4).getId() != r4,
               "group into folder: folder where the first rack was, members right after it");
        const auto fid = f->getId();
        const auto letter = f->busIndex.load();
        check (e.findRack (r1)->outputBus == letter && e.findRack (r3)->outputBus == letter && e.findRack (r1)->folderId == fid,
               "folder members play into the folder (its bus)");

        s.setFolderOpen (*f, false);
        check (s.isHiddenInFolder (*e.findRack (r1)) && ! s.isHiddenInFolder (*f) && s.numFolderMembers (*f) == 2, "closed folder hides its members");
        s.setFolderOpen (*f, true);

        // moving the folder takes its members along; a nested folder too
        s.moveRackTo (fid, e.getNumRacks() - 1);
        check (e.getRack (e.getNumRacks() - 3).getId() == fid && e.getRack (e.getNumRacks() - 1).getId() == r3,
               "moving a folder moves its members with it");
        auto* inner = s.groupIntoFolder ({ r3 });
        check (inner != nullptr && inner->folderId == fid && e.findRack (r3)->folderId == inner->getId()
                 && e.getRack (e.getNumRacks() - 1).getId() == r3 && inner->outputBus == letter,
               "folder inside a folder: plays into the outer one");

        s.removeFromFolder (*e.findRack (r1));
        check (e.findRack (r1)->folderId == -1 && e.findRack (r1)->outputBus == 0 && e.getRack (e.getNumRacks() - 1).getId() == r1,
               "remove from folder: back to the folder's output, after the folder");

        // saved and reloaded, then ungrouped
        const auto tree = s.toTree();
        AudioEngine e2;
        Session s2 (e2, *pm);
        s2.loadFromTree (tree, {}, false);
        auto* f2 = e2.findRack (fid);
        check (f2 != nullptr && f2->isFolder && e2.findRack (r3)->folderId == inner->getId()
                 && std::abs (e2.findRack (r3)->uiSendsV - 120.0f) < 0.01f && e2.findRack (r3)->uiInputV == 0.0f,
               "session keeps folders, members and each rack's dividers");

        const auto before = e.getNumRacks();
        s.ungroupFolder (*inner);
        check (e.getNumRacks() == before - 1 && e.findRack (r3)->folderId == fid && e.findRack (r3)->outputBus == letter,
               "ungroup: members go one level up, into the outer folder");
        check (s.undo() && e.getNumRacks() == before && e.findRack (r3)->folderId != fid, "ungroup is undoable");
    }

    // --- app settings saved in the session (features, audio device...): written by the app at
    // new racks: mono, next free input, output = the same channel; picking an input moves the output
    {
        AudioEngine e;
        e.prepare (48000.0, 64, {});
        auto* pm = new PluginManager();   // leaked on purpose
        Session s (e, *pm);
        s.newSession();
        auto& first = e.getRack (0);
        auto& r = s.addRack();
        check (! first.stereoInput.load() && ! first.stereoOutput.load() && first.inputChannel.load() == 0 && first.outputChannel.load() == 0
                 && ! r.stereoInput.load() && ! r.stereoOutput.load() && r.inputChannel.load() == 1 && r.outputChannel.load() == 1,
               "new racks are mono, IN n -> OUT n");

        r.inputChannel = 6; r.stereoInput = true;
        Session::followInput (r, e.isRoutingActive());
        const bool followed = r.outputChannel.load() == 6 && r.stereoOutput.load();
        r.outputChannel = -1; r.inputChannel = 2;
        Session::followInput (r, e.isRoutingActive());
        check (followed && r.outputChannel.load() == -1, "output follows the input (not when OUT is none)");
    }

    // save time, handed back after a load, kept as they were without the app
    {
        AudioEngine e;
        e.prepare (48000.0, 64, {});
        auto* pm = new PluginManager();   // leaked on purpose (its destructor saves files)
        Session s (e, *pm);
        s.captureAppSettings = []
        {
            juce::ValueTree t ("APPSETTINGS");
            t.setProperty ("feature_automix", false, nullptr);
            juce::ValueTree dev ("DEVICESETUP");
            dev.setProperty ("audioOutputDeviceName", "Dante Virtual Soundcard", nullptr);
            t.appendChild (dev, nullptr);
            return t;
        };
        const auto tree = s.toTree();

        AudioEngine e2;
        e2.prepare (48000.0, 64, {});
        Session s2 (e2, *pm);
        juce::ValueTree applied;
        s2.applyAppSettings = [&applied] (const juce::ValueTree& t) { applied = t.createCopy(); };
        s2.loadFromTree (tree, {}, false);
        check (applied.isValid() && ! (bool) applied["feature_automix"]
                 && applied.getChildWithName ("DEVICESETUP")["audioOutputDeviceName"].toString() == "Dante Virtual Soundcard",
               "session brings back its app settings (features, audio device)");
        s2.applyAppSettings = nullptr;
        check (s2.toTree().getChildWithName ("APPSETTINGS").isEquivalentTo (tree.getChildWithName ("APPSETTINGS")),
               "without the app the session's app settings are written back unchanged");
        s2.newSession();
        check (! s2.toTree().getChildWithName ("APPSETTINGS").isValid(), "a new session has no app settings of its own");
    }
    {
        // True bypass: a bypassed plugin is not called (after a fade and a drain), its hop
        // phase is kept on resume, and the crossfades have no jumps.
        struct CountingPlugin final : juce::AudioPluginInstance
        {
            int calls = 0;
            float tail = 0.0f;
            const juce::String getName() const override                        { return "Counting Test"; }
            void prepareToPlay (double, int) override                            {}
            void releaseResources() override                                     {}
            void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
            {
                ++calls;
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const auto x = b.getSample (0, i);
                    tail = 0.95f * tail + 0.05f * x;          // a "reverb": rings after the input stops
                    for (int ch = 0; ch < b.getNumChannels(); ++ch)
                        b.setSample (ch, i, 0.5f * x + tail);
                }
            }
            void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override   { ++calls; }
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

        AudioEngine e;
        auto& r = e.addRack();
        r.inputChannel = 0; r.stereoInput = false; r.outputChannel = 0;
        auto plugin = std::make_unique<CountingPlugin>();
        auto* cp = plugin.get();
        Rack::configurePlugin (*plugin, sr, bs);
        r.setPlugin (0, std::move (plugin));
        e.prepare (sr, bs, {});
        auto& slot = r.getSlot (0);

        const auto offline = cp->calls;                      // prepare's hop detection runs it offline
        runBlocks (e, 100, 0.5f);
        check (cp->calls - offline == 100 && near (out.getSample (0, bs - 1), 0.75f), "true bypass: plugin runs while on (calls " + juce::String (cp->calls) + ", out " + juce::String (out.getSample (0, bs - 1)) + ")");

        slot.hopCalls = 4;                                   // as if aligned: hop every 4 callbacks
        slot.hopPhase = 0;
        const auto before = cp->calls;
        slot.bypassed = true;

        float maxJump = 0.0f, last = out.getSample (0, bs - 1);
        for (int k = 0; k < 20; ++k)                         // fade (10 ms = 7.5 blocks of 64)
        {
            runBlocks (e, 1, 0.5f);
            for (int i = 0; i < bs; ++i) { maxJump = juce::jmax (maxJump, std::abs (out.getSample (0, i) - last)); last = out.getSample (0, i); }
        }
        check (near (out.getSample (0, bs - 1), 0.5f) && maxJump < 0.01f, "true bypass: dry after a click-free fade");

        runBlocks (e, 2000, 0.5f);                           // drain (0.3 s of quiet output) then idle
        const auto idleCalls = cp->calls;
        runBlocks (e, 1001, 0.5f);
        check (cp->calls == idleCalls && slot.bypassState == Rack::Slot::bpIdle && near (out.getSample (0, bs - 1), 0.5f),
               "true bypass: not called at all once idle (was " + juce::String (idleCalls - before) + " calls of fade + drain)");
        check (cp->tail < 1.0e-4f, "true bypass: its tail was drained with silence, nothing stale on resume");

        // a flush (Session::flushIdlePlugins): one call, only in phase, then idle again
        slot.flushWanted = true;
        runBlocks (e, 8, 0.5f);
        check (cp->calls == idleCalls + 1 && ! slot.flushWanted && slot.bypassState == Rack::Slot::bpIdle
                 && near (out.getSample (0, bs - 1), 0.5f), "true bypass: flush = one silent call while idle, output untouched");

        // switched off in SETTINGS > Features: the plugin runs again with its own bypass
        features::enabled[features::trueBypass] = false;
        runBlocks (e, 40, 0.5f);
        const auto offCalls = cp->calls;
        runBlocks (e, 100, 0.5f);
        check (cp->calls == offCalls + 100 && slot.bypassState == Rack::Slot::bpActive && near (out.getSample (0, bs - 1), 0.5f),
               "true bypass off: the plugin's own bypass, every callback");
        features::enabled[features::trueBypass] = true;
        runBlocks (e, 2100, 0.5f);
        check (slot.bypassState == Rack::Slot::bpIdle && near (out.getSample (0, bs - 1), 0.5f), "true bypass on again: idle");

        slot.bypassed = false;
        maxJump = 0.0f; last = out.getSample (0, bs - 1);
        for (int k = 0; k < 40; ++k)
        {
            runBlocks (e, 1, 0.5f);
            for (int i = 0; i < bs; ++i) { maxJump = juce::jmax (maxJump, std::abs (out.getSample (0, i) - last)); last = out.getSample (0, i); }
        }
        const auto cb = (juce::uint64) e.getCallbackCount();
        check ((juce::uint64) (cp->calls - offline) % 4 == cb % 4 && slot.bypassState == Rack::Slot::bpActive,
               "true bypass: on resume the missed calls are caught up (hop phase kept: calls "
                 + juce::String (cp->calls) + ", callbacks " + juce::String ((juce::int64) cb) + ")");
        check (maxJump < 0.02f && out.getSample (0, bs - 1) > 0.7f, "true bypass: click-free fade back in");
    }

    {
        // A plugin with latency: bypassed, the dry signal comes out as late as the plugin's
        // (the compensation counts it), without calling it.
        struct DelayPlugin final : juce::AudioPluginInstance
        {
            int calls = 0;
            std::vector<float> line = std::vector<float> (48, 0.0f);
            int pos = 0;
            DelayPlugin()                                                        { setLatencySamples (48); }
            const juce::String getName() const override                        { return "Delay Test"; }
            void prepareToPlay (double, int) override                            {}
            void releaseResources() override                                     {}
            void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
            {
                ++calls;
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const auto x = b.getSample (0, i), y = line[(size_t) pos];
                    line[(size_t) pos] = x;
                    pos = (pos + 1) % 48;
                    for (int ch = 0; ch < b.getNumChannels(); ++ch)
                        b.setSample (ch, i, 0.5f * y);   // delayed, -6 dB
                }
            }
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

        AudioEngine e;
        auto& r = e.addRack();
        r.inputChannel = 0; r.stereoInput = false; r.outputChannel = 0;
        auto plugin = std::make_unique<DelayPlugin>();
        auto* dp = plugin.get();
        Rack::configurePlugin (*plugin, sr, bs);
        r.setPlugin (0, std::move (plugin));
        e.prepare (sr, bs, {});
        r.getSlot (0).bypassed = true;
        runBlocks (e, 1000, 0.0f);                           // fade, drain, idle
        const auto idleCalls = dp->calls;
        runBlocks (e, 1, 0.0f, true);                        // impulse at sample 0
        check (dp->calls == idleCalls && std::abs (out.getSample (0, 48)) > 0.9f && std::abs (out.getSample (0, 0)) < 1.0e-6f,
               "true bypass with latency: not called, dry delayed by the plugin's 48 samples");
    }
}

} // namespace stagerack
