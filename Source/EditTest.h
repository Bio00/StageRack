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
        r->getSlot (0).viewMode = 0;   // parameter rows (new plugins default to the mini view)
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
    const juce::ModifierKeys shift (juce::ModifierKeys::shiftModifier | juce::ModifierKeys::leftButtonModifier);

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
    a.stereoInput = true; c.stereoInput = false;   // mixed widths: only the clicked rack takes the picked one
    session.setConsecutiveInputs ({ a.getId(), b.getId(), c.getId() }, false, 2, b.getId());
    check (b.inputChannel == 2 && c.inputChannel == 3 && a.inputChannel == 4 && ! b.stereoInput && ! c.stereoInput && a.stereoInput,
           "consecutive inputs, mono picked on B at 3, C mono, A stereo: B 3, C 4, A 5-6");
    session.setConsecutiveInputs ({ a.getId(), b.getId(), c.getId() }, true, 0, b.getId());
    check (b.inputChannel == 0 && c.inputChannel == 2 && a.inputChannel == 3 && b.stereoInput && ! c.stereoInput && a.stereoInput,
           "consecutive inputs, stereo picked on B at 1: B 1-2, C 3 mono, A 4-5");
    c.stereoOutput = false;
    session.setConsecutiveOutputs ({ b.getId(), c.getId() }, true, 0, b.getId());
    check (b.outputChannel == 0 && c.outputChannel == 2 && b.stereoOutput && ! c.stereoOutput, "consecutive outputs, mixed: B 1-2, C 3 mono");

    // --- plugin power (Alt-click on B's slot): same plugin in C switches too; selection kept
    auto slotOf = [&] (RackComponent& rc, int n) -> juce::Component*
    {
        // the slots live in a viewport (they scroll when they don't fit)
        juce::Component* holder = &rc;
        for (auto* ch : rc.getChildren())
            if (auto* vp = dynamic_cast<juce::Viewport*> (ch); vp != nullptr && vp->isVisible() && vp->getViewedComponent() != nullptr
                                                               && vp->getViewedComponent()->getNumChildComponents() == Rack::maxSlots)
                holder = vp->getViewedComponent();

        int k = 0;
        for (auto* ch : holder->getChildren())
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
            if (ch->isVisible() && ch->getWidth() > 30 && ch->getHeight() < 30 && ch->getY() > 15)   // narrow slots: two-line rows
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

    // --- Alt-click = default, Cmd = fine (ParamSlider + parameter rows)
    {
        const juce::ModifierKeys cmd (juce::ModifierKeys::commandModifier | juce::ModifierKeys::leftButtonModifier);
        auto* gs = dynamic_cast<juce::Slider*> (findChild (*rcB, [] (auto& ch) { return dynamic_cast<juce::Slider*> (&ch) != nullptr; }));
        const auto mid = gs->getLocalBounds().getCentre().toFloat();
        b.gainDb = -20.0f; c.gainDb = -1.0f; gs->setValue (-20.0, juce::dontSendNotification);

        setMods (alt);
        mouse (*gs, mid, mid, alt, 0);
        mouse (*gs, mid, mid, alt, 2);
        check (std::abs (b.gainDb.load()) < 0.01f && std::abs (c.gainDb.load()) < 0.01f && std::abs (a.gainDb - -5.0f) > 0.0f,
               "Alt-click gain on B: B and C back to 0 dB (B " + juce::String (b.gainDb.load(), 1) + ", C " + juce::String (c.gainDb.load(), 1) + ")");

        setMods (alt);
        mouse (*gs, mid, mid, alt, 0);
        mouse (*gs, mid.translated (0.0f, 30.0f), mid, alt, 1);
        mouse (*gs, mid.translated (0.0f, 30.0f), mid, alt, 2);
        check (b.gainDb < -0.5f && std::abs (c.gainDb - b.gainDb) < 0.01f, "Alt-drag gain still moves B and C together (" + juce::String (b.gainDb.load(), 1) + ")");

        auto dragBy = [&] (juce::ModifierKeys m, float dy)
        {
            gs->setValue (0.0, juce::sendNotificationSync);
            setMods (m);
            mouse (*gs, mid, mid, m, 0);
            mouse (*gs, mid.translated (0.0f, dy), mid, m, 1);
            mouse (*gs, mid.translated (0.0f, dy), mid, m, 2);
            setMods ({});
            return gs->getValue();
        };
        const auto coarse = dragBy (plain, 20.0f);
        const auto fine = dragBy (cmd, 20.0f);
        check (coarse < -1.0 && fine < -0.1 && std::abs (fine) < std::abs (coarse) * 0.4,
               "Cmd-drag gain is finer (20 px: " + juce::String (coarse, 2) + " dB plain, " + juce::String (fine, 2) + " dB with Cmd)");

        // Cmd pressed in the middle of a drag: no jump
        gs->setValue (0.0, juce::sendNotificationSync);
        setMods (plain);
        mouse (*gs, mid, mid, plain, 0);
        mouse (*gs, mid.translated (0.0f, 20.0f), mid, plain, 1);
        const auto atSwitch = gs->getValue();
        setMods (cmd);
        mouse (*gs, mid.translated (0.0f, 21.0f), mid, cmd, 1);
        const auto afterSwitch = gs->getValue();
        mouse (*gs, mid.translated (0.0f, 21.0f), mid, cmd, 2);
        setMods ({});
        check (std::abs (afterSwitch - atSwitch) < 0.2, "Cmd mid-drag doesn't jump (" + juce::String (atSwitch, 2) + " -> " + juce::String (afterSwitch, 2) + ")");

        if (auto* slot = slotOf (*rcB, 0))
        {
            juce::Component* row = nullptr;
            for (auto* ch : slot->getChildren())
                if (ch->isVisible() && ch->getWidth() > 30 && ch->getHeight() < 30 && ch->getY() > 15)   // narrow slots: two-line rows
                    row = ch;

            if (row != nullptr)
            {
                auto* pb = b.getPlugin (0);
                auto* pc = c.getPlugin (0);
                const juce::Point<float> start (5.0f, 8.0f);

                // find the parameter of the row, move it far from its default in B and C
                juce::Array<float> before;
                for (auto* prm : pb->getParameters()) before.add (prm->getValue());
                setMods (alt);
                mouse (*row, start, start, alt, 0);
                mouse (*row, start.translated (-40.0f, 0.0f), start, alt, 1);
                mouse (*row, start.translated (-40.0f, 0.0f), start, alt, 2);
                int idx = -1;
                for (int i = 0; i < pb->getParameters().size(); ++i)
                    if (std::abs (pb->getParameters()[i]->getValue() - before[i]) > 0.001f) idx = i;

                if (idx >= 0)
                {
                    auto* prmB = pb->getParameters()[idx];
                    auto* prmC = pc->getParameters()[idx];
                    prmB->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, prmB->getDefaultValue() + 0.3f > 1.0f ? prmB->getDefaultValue() - 0.3f : prmB->getDefaultValue() + 0.3f));
                    prmC->setValueNotifyingHost (prmB->getValue());
                    mouse (*row, start, start, alt, 0);
                    mouse (*row, start, start, alt, 2);
                    check (std::abs (prmB->getValue() - prmB->getDefaultValue()) < 0.01f && std::abs (prmC->getValue() - prmC->getDefaultValue()) < 0.01f,
                           "Alt-click parameter row: " + prmB->getName (32) + " back to default in B and C");

                    auto rowDrag = [&] (juce::ModifierKeys m)
                    {
                        prmB->setValueNotifyingHost (0.0f);
                        setMods (m);
                        mouse (*row, start, start, m, 0);
                        mouse (*row, start.translated (20.0f, 0.0f), start, m, 1);
                        mouse (*row, start.translated (20.0f, 0.0f), start, m, 2);
                        setMods ({});
                        return prmB->getValue();
                    };
                    const auto rc0 = rowDrag (plain), rf = rowDrag (cmd);
                    check (rc0 > 0.0f && rf > 0.0f && rf < rc0 * 0.4f, "Cmd-drag parameter row is finer (" + juce::String (rc0, 3) + " vs " + juce::String (rf, 3) + ")");
                }
                else
                {
                    check (false, "parameter row: no parameter moved");
                }
            }
        }

        // --- rack size border: Alt = selected racks the same size, Shift = same change
        juce::Component* handle = nullptr;
        for (auto* ch : rcB->getChildren())
            if (ch->isVisible() && (ch->getWidth() == 5 || ch->getHeight() == 5) && ch->getNumChildComponents() == 0
                && (ch->getRight() == rcB->getWidth() || ch->getBottom() == rcB->getHeight()) && dynamic_cast<juce::Slider*> (ch) == nullptr)
                handle = ch;

        check (handle != nullptr, "found the size border of B");

        if (handle != nullptr)
        {
            const juce::Point<float> p0 (2.0f, 2.0f);
            const bool horiz = handle->getWidth() == 5;
            const auto far = horiz ? p0.translated (60.0f, 0.0f) : p0.translated (0.0f, 60.0f);
            b.uiSize = 1.0f; c.uiSize = 1.5f; a.uiSize = 1.0f; d.uiSize = 1.0f;
            setMods (alt);
            mouse (*handle, p0, p0, alt, 0);
            mouse (*handle, far, p0, alt, 1);
            mouse (*handle, far, p0, alt, 2);
            check (b.uiSize > 1.05f && std::abs (c.uiSize - b.uiSize) < 0.001f && a.uiSize == 1.0f && d.uiSize == 1.0f,
                   "Alt-drag size border: C takes B's size (" + juce::String (b.uiSize, 2) + "), A/D untouched");

            b.uiSize = 1.0f; c.uiSize = 1.5f;
            setMods (shift);
            mouse (*handle, p0, p0, shift, 0);
            mouse (*handle, far, p0, shift, 1);
            mouse (*handle, far, p0, shift, 2);
            check (std::abs ((c.uiSize - 1.5f) - (b.uiSize - 1.0f)) < 0.001f && b.uiSize > 1.05f,
                   "Shift-drag size border: same change on C (B " + juce::String (b.uiSize, 2) + ", C " + juce::String (c.uiSize, 2) + ")");

            b.uiSize = 1.0f; c.uiSize = 1.5f;
            setMods (plain);
            mouse (*handle, p0, p0, plain, 0);
            mouse (*handle, far, p0, plain, 1);
            mouse (*handle, far, p0, plain, 2);
            check (c.uiSize == 1.5f && b.uiSize > 1.05f, "plain drag size border: B only");
            setMods ({});
            b.uiSize = c.uiSize = 1.0f;
        }

        // --- dividers inside the rack (vertical racks: input | plugins): Alt = same length, Shift = same change
        {
            for (int i = 0; i < comps.size(); ++i)
            {
                comps[i]->setVertical (true);
                comps[i]->setBounds (i * 200, 0, 190, 900);
            }
            paintAll();

            juce::Component* inHandle = nullptr;
            for (auto* ch : rcB->getChildren())
                if (auto* tt = dynamic_cast<juce::SettableTooltipClient*> (ch); tt != nullptr && ch->isVisible()
                    && tt->getTooltip().startsWith ("Drag: room for the input"))
                    inHandle = ch;
            check (inHandle != nullptr, "found the input divider of B (vertical)");

            if (inHandle != nullptr)
            {
                const juce::Point<float> p0 (2.0f, 2.0f), down (2.0f, 42.0f);
                auto dragDivider = [&] (juce::ModifierKeys m)
                {
                    for (auto* rc : comps) rc->resized();   // lay out the lengths set below
                    setMods (m);
                    mouse (*inHandle, p0, p0, m, 0);
                    mouse (*inHandle, down, p0, m, 1);
                    mouse (*inHandle, down, p0, m, 2);
                    setMods ({});
                };

                b.uiInputV = 50.0f; c.uiInputV = 80.0f; a.uiInputV = d.uiInputV = -1.0f;
                dragDivider (alt);
                check (std::abs (b.uiInputV - 90.0f) < 0.5f && std::abs (c.uiInputV - 90.0f) < 0.5f && a.uiInputV < 0.0f && d.uiInputV < 0.0f,
                       "Alt-drag input divider: C takes B's length (B " + juce::String (b.uiInputV, 0) + ", C " + juce::String (c.uiInputV, 0) + "), A/D untouched");

                b.uiInputV = 50.0f; c.uiInputV = 80.0f;
                dragDivider (shift);
                check (std::abs (b.uiInputV - 90.0f) < 0.5f && std::abs (c.uiInputV - 120.0f) < 0.5f,
                       "Shift-drag input divider: same change on C (B " + juce::String (b.uiInputV, 0) + ", C " + juce::String (c.uiInputV, 0) + ")");

                b.uiInputV = 50.0f; c.uiInputV = 80.0f;
                dragDivider (plain);
                check (std::abs (b.uiInputV - 90.0f) < 0.5f && std::abs (c.uiInputV - 80.0f) < 0.5f, "plain drag input divider: B only (B " + juce::String (b.uiInputV, 0) + ", C " + juce::String (c.uiInputV, 0) + ")");

                setMods (alt);
                inHandle->mouseDoubleClick (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p0, alt, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                              inHandle, inHandle, juce::Time::getCurrentTime(), p0, juce::Time::getCurrentTime(), 2, false));
                setMods ({});
                check (b.uiInputV < 0.0f && c.uiInputV < 0.0f, "Alt-double-click input divider: B and C back to automatic");
            }

            for (int i = 0; i < comps.size(); ++i)
            {
                comps[i]->setVertical (false);
                comps[i]->setBounds (0, i * RackComponent::baseHeight, 1400, RackComponent::baseHeight);
            }
            a.uiInputV = b.uiInputV = c.uiInputV = d.uiInputV = -1.0f;
            paintAll();
        }

        // leave the gains as the next checks expect them
        gs->setValue (-20.0, juce::dontSendNotification);
        b.gainDb = -20.0f; c.gainDb = -1.0f;
    }

    // --- Shift on a slot of a selected rack: plugin on/off on all selected racks (like Alt)
    using Link = EditController::Link;
    paintAll();
    slotB = slotOf (*rcB, 0);

    if (slotB != nullptr)
    {
        const juce::Point<float> power (2.0f + 5.0f + 9.0f, 2.0f + 4.0f + 9.0f);
        setMods (shift);
        mouse (*slotB, power, power, shift, 0);
        check (b.getSlot (0).bypassed && c.getSlot (0).bypassed && ! a.getSlot (0).bypassed && ! d.getSlot (0).bypassed,
               "Shift-click plugin power on B: off in B and C");
        mouse (*slotB, power, power, shift, 0);
        check (! b.getSlot (0).bypassed && ! c.getSlot (0).bypassed, "Shift-click again: back on");
    }

    check (edit.linkedSlots (b, 0, shift, Link::plugin).size() == 1 && edit.linkedSlots (b, 0, shift, Link::mix).empty(),
           "Shift links plugin insert/remove/on-off only (not gain or parameters: Shift = fine there)");

    // --- edit groups: A and D in group A, nothing selected
    setMods ({});
    edit.clearSelection();
    a.editGroup = d.editGroup = 1;
    a.muted = d.muted = false;
    a.gainDb = 0.0f; d.gainDb = -3.0f;
    paintAll();
    auto* gainA = dynamic_cast<juce::Slider*> (findChild (*comps[0], [] (auto& ch) { return dynamic_cast<juce::Slider*> (&ch) != nullptr; }));
    gainA->setValue (0.0, juce::dontSendNotification);
    gainA->setValue (-5.0, juce::sendNotificationSync);
    check (std::abs (a.gainDb - -5.0f) < 0.01f && std::abs (d.gainDb - -8.0f) < 0.01f && std::abs (b.gainDb - -20.0f) < 0.01f,
           "group gain: D follows relatively (-3 -> " + juce::String (d.gainDb.load(), 1) + "), B untouched");

    press (button (*comps[0], "M"), plain);
    check (a.muted && d.muted && ! b.muted, "group mute: A and D");
    press (button (*comps[0], "M"), plain);

    const auto dIn = d.inputChannel.load();
    auto* inA = dynamic_cast<juce::ComboBox*> (findChild (*comps[0], [] (auto& ch) { return dynamic_cast<juce::ComboBox*> (&ch) != nullptr; }));
    inA->setSelectedId (1, juce::dontSendNotification);
    inA->onChange();
    check (a.inputChannel < 0 && d.inputChannel == dIn, "group never re-routes: input of A only");

    if (auto* slotA = slotOf (*comps[0], 0))
    {
        const juce::Point<float> power (2.0f + 5.0f + 9.0f, 2.0f + 4.0f + 9.0f);
        mouse (*slotA, power, power, plain, 0);
        check (a.getSlot (0).bypassed && d.getSlot (0).bypassed && ! b.getSlot (0).bypassed, "group plugin power: A and D off");
        mouse (*slotA, power, power, plain, 0);
    }

    session.setEditGroupActive (1, false);
    press (button (*comps[0], "M"), plain);
    check (a.muted && ! d.muted, "group suspended: mute A only");
    press (button (*comps[0], "M"), plain);
    session.setEditGroupActive (1, true);

    {
        const auto t = session.toTree();
        bool saved = false;
        for (const auto& rt : t.getChildWithName ("RACKS"))
            saved = saved || (int) rt["editGroup"] == 1;
        check (saved && t.hasProperty ("groupsOff"), "edit groups saved with the session");
    }

    // a parameter moved in A's plugin window (not on the rack): D's AUDelay follows
    {
        auto* pa = a.getPlugin (0);
        auto* pdd = d.getPlugin (0);
        auto* pbb = b.getPlugin (0);
        int idx = -1;
        for (int n = 0; n < pa->getParameters().size() && idx < 0; ++n)
            if (pa->getParameters()[n]->isAutomatable())
                idx = n;

        edit.syncGroupParameters();   // first look: snapshots
        const auto bBefore = pbb->getParameters()[idx]->getValue();
        const auto target = pa->getParameters()[idx]->getValue() > 0.5f ? 0.2f : 0.8f;
        pa->getParameters()[idx]->setValueNotifyingHost (target);
        edit.syncGroupParameters();
        check (std::abs (pdd->getParameters()[idx]->getValue() - pa->getParameters()[idx]->getValue()) < 0.001f
                 && std::abs (pbb->getParameters()[idx]->getValue() - bBefore) < 0.001f,
               "plugin window edit on A (" + pa->getParameters()[idx]->getName (24) + "): D follows, B (other group) untouched");

        // both changed at once (e.g. a scene): nothing is mirrored
        pa->getParameters()[idx]->setValueNotifyingHost (0.3f);
        pdd->getParameters()[idx]->setValueNotifyingHost (0.6f);
        edit.syncGroupParameters();
        check (std::abs (pa->getParameters()[idx]->getValue() - pdd->getParameters()[idx]->getValue()) > 0.1f,
               "two instances changed together: left alone");

        // suspended group: no mirroring
        session.setEditGroupActive (1, false);
        edit.syncGroupParameters();
        pa->getParameters()[idx]->setValueNotifyingHost (0.9f);
        edit.syncGroupParameters();
        check (std::abs (pdd->getParameters()[idx]->getValue() - 0.6f) < 0.01f, "suspended group: plugin window edits stay on A");
        session.setEditGroupActive (1, true);
    }

    // delete the selected slot of A: D's AUDelay goes too, in one undo step
    edit.selectSlot (a.getId(), 0);
    edit.deleteSelection();
    check (a.getPlugin (0) == nullptr && d.getPlugin (0) == nullptr && b.getPlugin (0) != nullptr, "group remove: A and D, B kept");
    session.undo();
    check (a.getPlugin (0) != nullptr && d.getPlugin (0) != nullptr, "one undo brings both back");

    setMods ({});
    std::cout << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED") << std::endl;
    std::fflush (nullptr);
    std::_Exit (failures == 0 ? 0 : 1);   // plugins/windows stay alive (VST3/AU teardown off the message loop)
}

} // namespace stagerack
