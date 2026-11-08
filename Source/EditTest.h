#pragma once

#include "Engine/Session.h"
#include "UI/EditController.h"
#include "UI/RackComponent.h"
#include <iostream>

namespace stagerack
{

/** "StageRack --edit-selftest": the "Alt = all selected racks" editing, driven through the real rack
    components (offscreen window, synthetic mouse events and modifier keys). Uses Apple's AUDelay.
    Exits with _Exit: never writes the plugin list or the settings. */
inline int runEditSelfTest()
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
    EditController edit (session);

    auto& a = engine.addRack();
    auto& b = engine.addRack();
    auto& c = engine.addRack();
    auto& d = engine.addRack();               // not selected: must never change

    // the same plugin in slot 0 of every rack
    juce::PluginDescription delay;
    {
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : pm->getFormatManager().getFormats())
            if (f->getName() == "AudioUnit")
                f->findAllTypesForFile (found, "AudioUnit:Effects/aufx,dely,appl");

        if (found.isEmpty())
        {
            std::cout << "  SKIP  AUDelay not found" << std::endl;
            std::fflush (nullptr);
            std::_Exit (1);
        }
        delay = *found[0];
    }

    for (auto* r : { &a, &b, &c, &d })
    {
        juce::String err;
        auto p = pm->getFormatManager().createPluginInstance (delay, sr, bs, err);
        Rack::configurePlugin (*p, sr, bs);
        r->setPlugin (0, std::move (p));
    }

    engine.prepare (sr, bs, {});

    // the real components, in an offscreen window (they only build parameter rows while showing)
    juce::Component window;
    window.setBounds (-6000, -6000, 1400, 4 * RackComponent::baseHeight);
    juce::OwnedArray<RackComponent> comps;

    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        auto* rc = comps.add (new RackComponent (session, edit, engine.getRack (i), i));
        rc->setBounds (0, i * RackComponent::baseHeight, 1400, RackComponent::baseHeight);
        window.addAndMakeVisible (rc);
    }

    window.addToDesktop (0);
    window.setVisible (true);

    auto paintAll = [&]
    {
        for (auto* rc : comps)
        {
            rc->updateMeters();
            juce::ignoreUnused (rc->createComponentSnapshot (rc->getLocalBounds()));   // paint() computes hit areas
            rc->updateMeters();
        }
    };
    paintAll();

    // select B and C
    edit.clickRack (b.getId(), {});
    edit.clickRack (c.getId(), juce::ModifierKeys (juce::ModifierKeys::commandModifier));
    check (edit.isRackSelected (b.getId()) && edit.isRackSelected (c.getId()) && ! edit.isRackSelected (a.getId()), "selection B+C");

    const juce::ModifierKeys alt (juce::ModifierKeys::altModifier | juce::ModifierKeys::leftButtonModifier);
    const juce::ModifierKeys altShift (alt.getRawFlags() | juce::ModifierKeys::shiftModifier);
    const juce::ModifierKeys plain (juce::ModifierKeys::leftButtonModifier);

    auto* rcB = comps[1];

    auto findChild = [] (juce::Component& parent, auto pred) -> juce::Component*
    {
        for (auto* ch : parent.getChildren())
            if (pred (*ch))
                return ch;
        return nullptr;
    };

    auto setMods = [] (juce::ModifierKeys m) { juce::ModifierKeys::currentModifiers = m; };

    // --- gain: Alt = same value, Alt+Shift = relative, no Alt = this rack only
    auto* gainSlider = dynamic_cast<juce::Slider*> (findChild (*rcB, [] (auto& ch) { return dynamic_cast<juce::Slider*> (&ch) != nullptr; }));
    setMods (alt);
    gainSlider->setValue (-10.0, juce::sendNotificationSync);
    check (b.gainDb == -10.0f && c.gainDb == -10.0f && a.gainDb == 0.0f && d.gainDb == 0.0f, "Alt gain: B and C = -10 dB, A/D untouched");

    c.gainDb = -4.0f;
    setMods (altShift);
    gainSlider->setValue (-7.0, juce::sendNotificationSync);
    check (std::abs (b.gainDb - -7.0f) < 0.01f && std::abs (c.gainDb - -1.0f) < 0.01f, "Alt+Shift gain: relative (+3 dB on both: B -7, C " + juce::String (c.gainDb.load(), 1) + ")");

    setMods ({});
    gainSlider->setValue (-20.0, juce::sendNotificationSync);
    check (b.gainDb == -20.0f && std::abs (c.gainDb - -1.0f) < 0.01f, "plain gain: B only");

    // --- mute/solo/bypass buttons
    auto button = [&] (RackComponent& rc, const juce::String& text)
    {
        return dynamic_cast<juce::TextButton*> (findChild (rc, [&] (auto& ch) { auto* tb = dynamic_cast<juce::TextButton*> (&ch); return tb != nullptr && tb->getButtonText() == text; }));
    };

    auto press = [&] (juce::TextButton* tb, juce::ModifierKeys m)
    {
        setMods (m);
        tb->setToggleState (! tb->getToggleState(), juce::dontSendNotification);
        tb->onClick();
    };

    press (button (*rcB, "M"), alt);
    check (b.muted && c.muted && ! a.muted && ! d.muted, "Alt mute: B and C muted");
    press (button (*rcB, "M"), plain);
    check (! b.muted && c.muted, "plain unmute: B only");
    press (button (*comps[2], "S"), alt);
    check (b.solo && c.solo && ! a.solo, "Alt solo from C: B and C");
    press (button (*rcB, "B"), alt);
    check (b.bypassed && c.bypassed && ! d.bypassed, "Alt rack bypass: B and C");

    // Alt on a rack that is NOT selected acts on that rack only
    press (button (*comps[0], "M"), alt);
    check (a.muted && ! d.muted && ! b.muted, "Alt on an unselected rack: only that rack");

    // --- input: Alt + "From rack A" on B feeds B and C from A
    auto* inBox = dynamic_cast<juce::ComboBox*> (findChild (*rcB, [] (auto& ch) { return dynamic_cast<juce::ComboBox*> (&ch) != nullptr; }));
    setMods (alt);
    inBox->setSelectedId (3000 + a.getId(), juce::dontSendNotification);
    inBox->onChange();
    check (b.inputRackId == a.getId() && c.inputRackId == a.getId() && d.inputRackId < 0, "Alt input 'from rack A': B and C chained to A");

    // ... and Alt + "IN: none" frees both again
    paintAll();
    inBox = dynamic_cast<juce::ComboBox*> (findChild (*rcB, [] (auto& ch) { return dynamic_cast<juce::ComboBox*> (&ch) != nullptr; }));
    inBox->setSelectedId (1, juce::dontSendNotification);
    inBox->onChange();
    check (b.inputRackId < 0 && c.inputRackId < 0 && b.inputChannel < 0 && c.inputChannel < 0, "Alt input 'none': B and C");

    // --- consecutive from the picked channel, starting at the clicked rack (C), wrapping to B
    session.setConsecutiveOutputs ({ b.getId(), c.getId() }, true, 4, c.getId());
    check (c.outputChannel == 4 && b.outputChannel == 6 && c.stereoOutput && b.stereoOutput, "consecutive outputs from C at 5-6: C 5-6, B 7-8");
    session.setConsecutiveInputs ({ a.getId(), b.getId(), c.getId() }, false, 2, b.getId());
    check (b.inputChannel == 2 && c.inputChannel == 3 && a.inputChannel == 4 && ! b.stereoInput, "consecutive mono inputs from B at 3: B 3, C 4, A 5");

    // --- plugin power (Alt-click on B's slot): same plugin in C switches too; selection kept
    auto slotOf = [&] (RackComponent& rc, int n) -> juce::Component*
    {
        int k = 0;
        for (auto* ch : rc.getChildren())
            if (dynamic_cast<juce::Slider*> (ch) == nullptr && dynamic_cast<juce::Button*> (ch) == nullptr
                && dynamic_cast<juce::ComboBox*> (ch) == nullptr && dynamic_cast<juce::Label*> (ch) == nullptr
                && ch->getNumChildComponents() > 0)   // slots hold their resizer (+ parameter rows)
                if (k++ == n)
                    return ch;
        return nullptr;
    };

    auto mouse = [] (juce::Component& comp, juce::Point<float> pos, juce::Point<float> downPos, juce::ModifierKeys m, int kind)
    {
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto now = juce::Time::getCurrentTime();
        juce::MouseEvent e (src, pos, m, juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                            juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY,
                            &comp, &comp, now, downPos, now, 1, kind == 1);
        if (kind == 0) comp.mouseDown (e);
        else if (kind == 1) comp.mouseDrag (e);
        else comp.mouseUp (e);
    };

    paintAll();
    auto* slotB = slotOf (*rcB, 0);
    check (slotB != nullptr, "found slot 0 of rack B");

    if (slotB != nullptr)
    {
        const juce::Point<float> power (2.0f + 5.0f + 9.0f, 2.0f + 4.0f + 9.0f);
        setMods (alt);
        mouse (*slotB, power, power, alt, 0);
        check (b.getSlot (0).bypassed && c.getSlot (0).bypassed && ! a.getSlot (0).bypassed && ! d.getSlot (0).bypassed,
               "Alt-click plugin power on B: AUDelay off in B and C");
        check (edit.isRackSelected (b.getId()) && edit.isRackSelected (c.getId()), "rack selection kept after Alt-click");
        mouse (*slotB, power, power, alt, 0);
        check (! b.getSlot (0).bypassed && ! c.getSlot (0).bypassed, "Alt-click again: back on in B and C");

        // --- parameter row: Alt-drag sets the same parameter in C's AUDelay
        juce::Component* row = nullptr;
        for (auto* ch : slotB->getChildren())
            if (ch->isVisible() && ch->getWidth() > 30 && ch->getHeight() < 20)
                row = ch;

        check (row != nullptr, "parameter row visible on B's slot");

        if (row != nullptr)
        {
            // which parameter does it show? find it by moving it
            auto* pb = b.getPlugin (0);
            auto* pc = c.getPlugin (0);
            auto* pd = d.getPlugin (0);
            juce::Array<float> before;
            for (auto* prm : pb->getParameters()) before.add (prm->getValue());

            const juce::Point<float> start (5.0f, 8.0f);
            setMods (alt);
            mouse (*row, start, start, alt, 0);
            mouse (*row, start.translated (-2000.0f, 0.0f), start, alt, 1);   // all the way down
            mouse (*row, start.translated (-2000.0f, 0.0f), start, alt, 2);

            int moved = -1;
            for (int i = 0; i < pb->getParameters().size(); ++i)
                if (std::abs (pb->getParameters()[i]->getValue() - before[i]) > 0.001f)
                    moved = i;

            check (moved >= 0, "Alt-drag moved a parameter of B ("
                                 + (moved >= 0 ? pb->getParameters()[moved]->getName (32) : juce::String ("none")) + ")");

            if (moved >= 0)
            {
                const auto v = pb->getParameters()[moved]->getValue();
                check (std::abs (pc->getParameters()[moved]->getValue() - v) < 0.001f, "same value in C's AUDelay");
                check (std::abs (pd->getParameters()[moved]->getValue() - before[moved]) < 0.001f, "D (unselected) untouched");

                // plain drag: only B
                const auto cBefore = pc->getParameters()[moved]->getValue();
                setMods (plain);
                mouse (*row, start, start, plain, 0);
                mouse (*row, start.translated (2000.0f, 0.0f), start, plain, 1);
                mouse (*row, start.translated (2000.0f, 0.0f), start, plain, 2);
                check (pb->getParameters()[moved]->getValue() > 0.99f && std::abs (pc->getParameters()[moved]->getValue() - cBefore) < 0.001f,
                       "plain drag: B only");
            }
        }
    }

    setMods ({});
    std::cout << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED") << std::endl;
    std::fflush (nullptr);
    std::_Exit (failures == 0 ? 0 : 1);   // plugins/windows stay alive (VST3/AU teardown off the message loop)
}

} // namespace stagerack
