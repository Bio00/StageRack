#pragma once

#include "Engine/Session.h"
#include <iostream>

namespace stagerack
{

/** "StageRack --undo-selftest": undo/redo of rack and plugin edits with real AUDelay instances.
    Checks that undo puts back the very same instances (no reload). Exits with _Exit. */
inline int runUndoSelfTest()
{
    int failures = 0;
    auto check = [&] (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
        failures += ok ? 0 : 1;
    };

    const double sr = 48000.0;
    const int bs = 64;
    AudioEngine engine;
    auto* pm = new PluginManager();          // leaked on purpose: its destructor saves files
    Session session (engine, *pm);
    engine.prepare (sr, bs, {});
    session.newSession();

    juce::PluginDescription delay;
    {
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : pm->getFormatManager().getFormats())
            if (f->getName() == "AudioUnit")
                f->findAllTypesForFile (found, "AudioUnit:Effects/aufx,dely,appl");
        if (found.isEmpty()) { std::cout << "  SKIP  AUDelay not found" << std::endl; std::fflush (nullptr); std::_Exit (1); }
        delay = *found[0];
    }

    auto make = [&]
    {
        juce::String err;
        auto p = pm->getFormatManager().createPluginInstance (delay, sr, bs, err);
        Rack::configurePlugin (*p, sr, bs);
        return p;
    };

    juce::AudioBuffer<float> in (2, bs), out (2, bs);
    juce::AudioIODeviceCallbackContext ctx;
    auto run = [&] { for (int k = 0; k < 8; ++k) engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 2, bs, ctx); };

    auto& a = engine.getRack (0);
    const int idA = a.getId();
    auto* B = &session.addRack();                                   // 1
    const int idB = B->getId();
    auto upA = make(); auto* pA = upA.get();
    a.setPlugin (0, std::move (upA)); session.markDirty();          // 2
    auto upB = make(); auto* pB = upB.get();
    B->setPlugin (0, std::move (upB)); session.markDirty();         // 3
    B->gainDb = -10.0f; session.markDirty();                        // 4
    session.movePlugin (a, 0, a, 3);                                // 5
    session.movePlugin (a, 3, *B, 1);                               // 6
    session.removePlugin (*B, 0);                                   // 7
    session.addScene ("S1");                                        // 8
    session.removeRack (*B);                                        // 9
    run();

    check (engine.getNumRacks() == 1 && session.getNumUndoSteps() == 9, "9 steps recorded (" + juce::String (session.getNumUndoSteps()) + ")");

    auto rackB = [&] { return engine.findRack (idB); };
    auto at = [&] (int id, int s) { auto* r = engine.findRack (id); return r != nullptr ? r->getPlugin (s) : nullptr; };

    session.undo(); run();
    check (rackB() != nullptr && at (idB, 1) == pA && rackB()->gainDb == -10.0f && engine.indexOf (*rackB()) == 1,
           "undo rack delete: B back, same instance in slot 2, gain -10, second place");
    session.undo(); run();
    check (session.getNumScenes() == 0, "undo add scene");
    session.undo(); run();
    check (at (idB, 0) == pB, "undo remove: the same parked instance is back in B slot 1");
    session.undo(); run();
    check (at (idA, 3) == pA && at (idB, 1) == nullptr, "undo move between racks: back in A slot 4");
    session.undo(); run();
    check (at (idA, 0) == pA && at (idA, 3) == nullptr, "undo move inside the rack: back in A slot 1");
    session.undo(); run();
    check (rackB()->gainDb == 0.0f, "undo gain");
    session.undo(); session.undo(); run();
    check (at (idA, 0) == nullptr && at (idB, 0) == nullptr, "undo inserts: slots empty again");
    session.undo(); run();
    check (engine.getNumRacks() == 1 && rackB() == nullptr && ! session.canUndo(), "undo add rack: back to the start, nothing left to undo");

    for (int i = 0; i < 9; ++i) { session.redo(); run(); }
    check (engine.getNumRacks() == 1 && at (idA, 0) == nullptr && session.getNumScenes() == 1 && ! session.canRedo(),
           "redo all: same end state");

    for (int i = 0; i < 3; ++i) { session.undo(); run(); }
    check (at (idB, 0) == pB && at (idB, 1) == pA && session.getNumScenes() == 0, "undo x3 after redo: instances again");

    // a new edit drops the redo branch
    session.getEngine().getRack (0).gainDb = -3.0f; session.markDirty();
    check (! session.canRedo(), "new edit clears redo");

    // the parked instances still work: process with them
    run();
    check (out.getNumSamples() == bs, "audio runs after all of it");

    std::cout << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED") << std::endl;
    std::fflush (nullptr);
    std::_Exit (failures == 0 ? 0 : 1);
}

} // namespace stagerack
