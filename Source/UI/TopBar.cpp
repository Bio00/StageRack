#include "../Plugins/PluginManager.h"
#include "../Engine/SystemHealth.h"
#include "TopBar.h"
#include "../Engine/MidiControl.h"
#include "../Engine/CoreAudioProbe.h"
#include "Theme.h"
#include "RackViewMenu.h"

namespace stagerack
{

TopBar::TopBar (Session& s) : session (s)
{
    groupsButton.setTooltip ("Edit groups window: racks of an active group follow each other's gain, mute/solo/bypass and plugins (Cmd+Shift+G = all on/off)");
    groupsButton.onClick = [this] { if (onGroupsView) onGroupsView(); };
    midiMapButton.setTooltip ("MIDI map mode (Cmd+M): click a control, then move a knob, fader or button on your controller");
    midiMapButton.onClick = [this] { if (onMidiMap) onMidiMap(); };
    midiButton.setTooltip ("MIDI window (Cmd+Alt+M): controllers, mappings, ranges, LEDs, monitor");
    midiButton.onClick = [this] { if (onMidiWindow) onMidiWindow(); };
    addAndMakeVisible (midiMapButton);
    addAndMakeVisible (midiButton);
    tempoButton.setTooltip ("Tempo window: tempos, tap sources (key, MIDI, clock, audio), tap settings (Cmd+T)");
    tempoButton.onClick = [this] { if (onTempoView) onTempoView(); };
    addTempoButton.setTooltip ("Add a tap tempo (it follows the master until you tap it). Right click a tempo to remove it.");
    addTempoButton.onClick = [this] { session.addTempo(); rebuildTempoCells(); };
    for (auto* b : { &groupsButton, &tempoButton, &addTempoButton })
        addAndMakeVisible (*b);
    rebuildTempoCells();

    prevButton.onClick = [this] { session.stepScene (-1); };
    nextButton.onClick = [this] { session.stepScene (1); };
    prevButton.setTooltip ("Previous snapshot (Left arrow)");
    nextButton.setTooltip ("Next snapshot (Right arrow / Space)");

    storeButton.setTooltip ("Store current settings into the active snapshot (Cmd+Shift+S)");
    storeButton.onClick = [this]
    {
        if (session.getCurrentScene() >= 0)
            session.storeScene (session.getCurrentScene());
        else
            session.addScene ("Snapshot 1");
    };

    newButton.onClick      = [this] { if (onNew) onNew(); };
    openButton.onClick     = [this] { if (onOpen) onOpen(); };
    saveButton.onClick     = [this] { if (onSave) onSave(); };
    pluginsButton.onClick  = [this] { if (onPlugins) onPlugins(); };
    settingsButton.onClick = [this] { if (onSettings) onSettings(); };
    layoutButton.onClick   = [this] { if (onToggleLayout) onToggleLayout(); };
    layoutButton.setTooltip ("Switch racks between horizontal rows and vertical strips");
    themeButton.onClick    = [this] { if (onToggleTheme) onToggleTheme(); };
    themeButton.setTooltip ("Light / dark theme");
    newButton.setTooltip ("New session (Cmd+N)");
    openButton.setTooltip ("Open a session (Cmd+O)");
    saveButton.setTooltip ("Save the session (Cmd+S)");
    pluginsButton.setTooltip ("Plugin manager: scan, formats, latencies");
    settingsButton.setTooltip ("Audio device, buffer, MIDI and app settings (Cmd+,)");

    for (auto* b : { &prevButton, &nextButton, &storeButton, &newButton, &openButton, &saveButton, &pluginsButton, &settingsButton, &layoutButton, &themeButton, &viewButton })
        addAndMakeVisible (*b);

    viewButton.setButtonText (juce::String::fromUTF8 ("VIEW \xe2\x96\xbe"));
    viewButton.setTooltip ("What the racks show (Pro Tools style): I/O, input, inserts, sends (which buses), info. Saved with the session.");
    viewButton.onClick = [this]
    {
        rackViewMenu (session).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&viewButton));
    };

    storeButton.setColour (juce::TextButton::buttonColourId, theme::accent.withAlpha (0.25f));

    automixButton.setClickingTogglesState (true);
    automixButton.setColour (juce::TextButton::buttonOnColourId, theme::green.withAlpha (0.8f));
    automixButton.setTooltip ("Gain-sharing automixer on/off for all groups (racks join a group from the rack menu > Automix). Stored in snapshots.");
    automixButton.onClick = [this]
    {
        session.getEngine().automixEnabled = automixButton.getToggleState();
        session.markDirty();
    };
    addAndMakeVisible (automixButton);

    masterMuteButton.setClickingTogglesState (true);
    masterMuteButton.setColour (juce::TextButton::buttonOnColourId, theme::red);
    masterMuteButton.setTooltip ("Master mute: silences every output (the racks keep running). Cmd+Shift+M");
    masterMuteButton.onClick = [this] { session.getEngine().masterMuted = masterMuteButton.getToggleState(); };
    addAndMakeVisible (masterMuteButton);

    automixViewButton.setTooltip ("Automixer window: input level and automix gain of every mic, weight, group");
    automixViewButton.onClick = [this] { if (onAutomixView) onAutomixView(); };
    addAndMakeVisible (automixViewButton);
    themeButton.setButtonText (theme::light ? "DARK" : "LIGHT");
    setLayoutString (defaultLayoutString());
}

void TopBar::lookAndFeelChanged()
{

    storeButton.setColour (juce::TextButton::buttonColourId, theme::accent.withAlpha (0.25f));
    automixButton.setColour (juce::TextButton::buttonOnColourId, theme::green.withAlpha (0.8f));
    masterMuteButton.setColour (juce::TextButton::buttonOnColourId, theme::red);
    themeButton.setButtonText (theme::light ? "DARK" : "LIGHT");
}

void TopBar::setVerticalLayout (bool v)
{
    layoutButton.setButtonText (v ? "HORIZONTAL" : "VERTICAL");
}

void TopBar::refresh()
{
    storeButton.setColour (juce::TextButton::buttonColourId, theme::accent.withAlpha (0.25f));
    themeButton.setButtonText (theme::light ? "DARK" : "LIGHT");
    repaint();
}

void TopBar::rebuildTempoCells()
{
    tempoCells.clear();

    for (int i = 0; i < session.getEngine().tempo.getNumTempos(); ++i)
    {
        auto* c = tempoCells.add (new TempoCell (session, i));
        c->onOpenWindow = [this] { if (onTempoView) onTempoView(); };
        addAndMakeVisible (c);
    }

    addTempoButton.setEnabled (tempoCells.size() < TempoEngine::maxTempos);
    resized();
}

int TopBar::activeCores() const
{
    // multicore: the cores that did some work lately (the audio thread counts), at least 2
    if (! session.getEngine().isParallel())
        return 1;

    int n = 0;
    for (auto l : threadLoads)
        if (l > 0.005f)
            ++n;
    return juce::jmax (2, n);
}

double TopBar::secondsSinceLastDrop() const
{
    auto& engine = session.getEngine();
    const auto last = engine.dropouts.getLastDropCallback();
    if (last == 0)
        return 1.0e9;
    return (double) (engine.getCallbackCount() - last) * engine.getBlockSize() / juce::jmax (1.0, engine.getSampleRate());
}

juce::String TopBar::dropoutTooltip() const
{
    const auto& d = session.getEngine().dropouts;
    juce::String t;

    if (const auto& b = session.getEngine().budget; b.isActive())
    {
        auto& engine = session.getEngine();
        const auto sr = juce::jmax (1.0, engine.getSampleRate());
        const auto* rack = engine.findRack (b.getWorstRack());
        t << "BUFFER TOO SMALL: the plugins " << (rack != nullptr ? "of '" + rack->name + "' " : juce::String())
          << "needed up to " << juce::String (b.getWorstMs(), 2) << " ms, the buffer of " << b.getBuffer() << " samples gives "
          << juce::String (1000.0 * b.getBuffer() / sr, 2) << " ms (CoreAudio and the driver need part of it too).\n";
        if (b.getRecommended() > 0)
            t << "Raise it to " << b.getRecommended() << " samples (" << juce::String (1000.0 * b.getRecommended() / sr, 2)
              << " ms): click here. Save the session to keep it: a session brings back its own buffer.\n\n";
        else
            t << "The device offers no bigger buffer: lighten the chain (fewer plugins in series).\n\n";
    }

    if (const int cores = activeCores(); cores > 1)
        t << "MULTICORE: the racks run in parallel on " << cores << " cores (the work is more than about 30% of the buffer).\n\n";
    else
        t << "SINGLE CORE: all racks run on the audio thread. The load is light: waking the other cores would cost more\n"
             "than it saves. It goes multicore by itself when the work passes about 30% of the buffer.\n\n";

    t << "Dropouts since start (click to reset or see the log):\n"
      << "  late (processing longer than the buffer): " << (int) d.getLate() << "\n"
      << "  skipped (CoreAudio called us late / system stall): " << (int) d.getSkipped() << "\n"
      << "  silent (blocks lost while racks were changed): " << (int) d.getSilent() << "\n"
      << "Each one is written with its time and the heaviest rack to dropouts.log.";
    if (systemProblems.isNotEmpty())
        t << "\n\nThe Mac now: " << systemProblems
          << "\nLow Power Mode and heat slow the CPU down; low memory can stall the audio thread.";
    return t;
}

void TopBar::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.getEventRelativeTo (this).getPosition();

    if (customizing)
    {
        dragItem = -1;
        for (int it = 0; it < numItems; ++it)
            if (isItemShown (it) && itemArea[(size_t) it].contains (pos))
                dragItem = it;

        if (e.mods.isPopupMenu() && dragItem >= 0)
        {
            setItemHidden (dragItem, true);   // right click in edit mode: hide the block
            dragItem = -1;
        }

        dropIndex = -1;
        repaint();
        return;
    }

    if (e.mods.isPopupMenu())
    {
        showContextMenu (pos);
        return;
    }

    if (deviceArea.contains (pos))
    {
        if (onDeviceClicked) onDeviceClicked();
        return;
    }

    if (cpuArea.contains (pos))
        showContextMenu (pos);
}

void TopBar::showContextMenu (juce::Point<int> pos)
{
    juce::PopupMenu m;
    juce::Component::SafePointer<TopBar> safe (this);

    if (cpuArea.contains (pos))
    {
        if (const auto rec = session.getEngine().budget.getRecommended(); session.getEngine().budget.isActive() && rec > 0)
        {
            m.addItem ("Set the buffer to " + juce::String (rec) + " samples", [safe, rec]
            {
                if (safe == nullptr)
                    return;

                auto& dm = safe->session.getEngine().getDeviceManager();
                auto setup = dm.getAudioDeviceSetup();
                setup.bufferSize = rec;
                dm.setAudioDeviceSetup (setup, true);
            });
            m.addSeparator();
        }

        m.addItem ("Reset dropout counter", [safe]
        {
            if (safe != nullptr) { safe->session.getEngine().dropouts.clearCounters(); safe->repaint (safe->cpuArea); }
        });
        const auto log = PluginManager::getDataDirectory().getChildFile ("dropouts.log");
        m.addItem ("Show dropout log", log.existsAsFile(), false, [log] { log.revealToUser(); });
        m.addSeparator();
    }

    juce::PopupMenu show;
    for (auto it : order)
    {
        const auto f = itemFeature (it);
        const bool featureOn = f < 0 || features::has ((features::Feature) f);
        show.addItem (itemName (it) + (featureOn ? juce::String() : "  (off in Features)"), featureOn, isItemShown (it),
                      [safe, it] { if (safe != nullptr) safe->setItemHidden (it, ! safe->isItemHidden (it)); });
    }

    m.addSubMenu ("Show in the top bar", show);
    m.addItem ("Customize Top Bar...", [safe] { if (safe != nullptr) safe->showCustomizeWindow(); });
    m.addItem ("Reset Top Bar", [safe] { if (safe != nullptr) { safe->setLayoutString (defaultLayoutString()); safe->itemsChanged(); } });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                               .withTargetScreenArea (localAreaToGlobal (juce::Rectangle<int> (pos.x, pos.y, 1, 1))));
}

void TopBar::mouseMove (const juce::MouseEvent& e)
{
    juce::String tip;

    if (cpuArea.contains (e.getPosition()))
        tip = dropoutTooltip();

    if (deviceArea.contains (e.getPosition()))
        tip = "Click: audio device settings. Right click: customize the top bar";

    if (customizing)
        tip = "Drag a block to move it, right click to hide it";

    if (logoArea.contains (e.getPosition()))
        tip = session.getFile() == juce::File() ? juce::String ("Untitled session") : session.getFile().getFullPathName();

    if (tip != getTooltip())
        setTooltip (tip);
}

void TopBar::updateMeters()
{
    auto& engine = session.getEngine();
    const auto& pool = engine.getPool();
    const auto budgetMs = 1000.0 * engine.getBlockSize() / juce::jmax (1.0, engine.getSampleRate());

    threadLoads.resize ((size_t) pool.getNumThreads());

    for (int i = 0; i < pool.getNumThreads(); ++i)
    {
        const auto load = (float) juce::jlimit (0.0, 1.0, pool.getThreadBusyMs (i) / budgetMs);
        threadLoads[(size_t) i] = juce::jmax (load, threadLoads[(size_t) i] * 0.85f);
    }

    totalLoad = juce::jmax ((float) engine.getCallbackLoad(), totalLoad * 0.9f);

    // only meaningful when some rack is automixed
    bool anyAutomix = false;
    for (int i = 0; i < engine.getNumRacks() && ! anyAutomix; ++i)
        anyAutomix = engine.getRack (i).automixGroup.load() > 0;

    automixButton.setToggleState (engine.automixEnabled.load(), juce::dontSendNotification);
    automixButton.setAlpha (anyAutomix ? 1.0f : 0.4f);

    // master mute: blinks while on, so a silent PA is never a mystery
    const bool muted = engine.masterMuted.load();
    masterMuteButton.setToggleState (muted, juce::dontSendNotification);
    blink = muted ? (blink + 1) % 30 : 0;
    masterMuteButton.setColour (juce::TextButton::buttonOnColourId, blink < 15 ? theme::red : theme::red.darker (0.6f));

    // MIDI: map mode lit; the MIDI button flashes on controller activity
    if (auto* midi = session.midiControl)
    {
        midiMapButton.setColour (juce::TextButton::buttonColourId, midi->isMapMode() ? theme::midiBlue : theme::slot);
        midiMapButton.setColour (juce::TextButton::textColourOffId, midi->isMapMode() ? juce::Colours::white : theme::text);

        const auto now = juce::Time::getMillisecondCounter();
        if (midi->getEventCount() != midiEventsSeen)
        {
            midiEventsSeen = midi->getEventCount();
            midiActivityAt = now;
        }
        const bool active = now - midiActivityAt < 120;
        midiButton.setColour (juce::TextButton::buttonColourId, active ? theme::midiBlue.withAlpha (0.55f) : theme::slot);
    }

    if (tempoCells.size() != session.getEngine().tempo.getNumTempos())
        rebuildTempoCells();
    for (auto* c : tempoCells)
        c->repaintDisplay();

    // GROUPS lit while some edit group links racks
    bool groupsOn = false;
    for (int g = 1; g <= Rack::maxEditGroups && ! groupsOn; ++g)
        groupsOn = session.numRacksInEditGroup (g) > 0 && session.isEditGroupActive (g);
    groupsButton.setColour (juce::TextButton::buttonColourId, groupsOn ? theme::accent.withAlpha (0.45f) : theme::slot);

    // VIEW lit while the racks hide something
    viewButton.setColour (juce::TextButton::buttonColourId, session.getRackView() == Session::RackView() ? theme::slot : theme::accent.withAlpha (0.45f));

    if (++healthTick % 30 == 0)
        systemProblems = systemhealth::problems();

    // DSP bars (in pixels), totals, dropouts: repaint only what changed (an idle window draws nothing)
    {
        juce::String key;
        const int barH = juce::jmax (1, cpuArea.getHeight() - 32);
        for (auto l : threadLoads)
            key << juce::roundToInt (l * (float) barH) << ',';
        key << juce::roundToInt (totalLoad * 100.0f) << '|' << (int) session.getEngine().dropouts.getTotal()
            << '|' << juce::roundToInt (juce::jmin (61.0, secondsSinceLastDrop())) << '|' << systemProblems
            << '|' << (session.getEngine().hasWorkgroup() ? 1 : 0)
            << '|' << (session.getEngine().budget.isActive() ? session.getEngine().budget.getRecommended() : -1)
            << '|' << activeCores();

        if (key != shownCpuKey)
        {
            shownCpuKey = key;
            repaint (cpuArea);
        }
    }

    // the device lines change rarely (device, status, CoreAudio waiting): twice a second is plenty
    if (healthTick % 15 == 0)
        repaint (deviceArea);
}

void TopBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

    for (auto x : separators)
    {
        g.setColour (theme::outline.withAlpha (0.7f));
        g.drawVerticalLine (x, 12.0f, (float) getHeight() - 12.0f);
    }

    // logo + session file (top row of the session section)
    if (! logoArea.isEmpty())
    {
        auto row = logoArea;
        g.setColour (theme::text);
        g.setFont (theme::font (16.0f, true));
        g.drawText ("STAGE", row.removeFromLeft (52), juce::Justification::centredLeft);
        g.setColour (theme::accent);
        g.drawText ("RACK", row.removeFromLeft (44), juce::Justification::centredLeft);

        g.setColour (session.isDirty() ? theme::amber : theme::textDim);
        g.setFont (theme::font (11.0f, true));
        auto f = session.getFile();
        g.drawText ((f == juce::File() ? juce::String ("Untitled") : f.getFileNameWithoutExtension()) + (session.isDirty() ? " *" : ""),
                    row, juce::Justification::centredLeft, true);
    }

    // scene display
    if (! sceneArea.isEmpty())
    {
        auto r = sceneArea.toFloat();
        g.setColour (theme::display);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (theme::accent.withAlpha (0.6f));
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

        const auto cur = session.getCurrentScene();
        const auto num = cur >= 0 ? juce::String (cur + 1).paddedLeft ('0', 2) : juce::String ("--");
        const auto name = cur >= 0 ? session.getScene (cur).name : juce::String ("No snapshot");

        const bool small = r.getWidth() < 200.0f;   // narrow window: smaller number, more room for the name
        const float numW = small ? 36.0f : 60.0f;
        auto inner = r.reduced (small ? 8.0f : 12.0f, 0.0f);

        g.setColour (theme::accent);
        g.setFont (theme::font (small ? 22.0f : 30.0f, true));
        g.drawText (num, inner.removeFromLeft (numW), juce::Justification::centredLeft);

        g.setColour (theme::text);
        g.setFont (theme::font (small ? 16.0f : 19.0f, true));
        g.drawFittedText (name, inner.withTrimmedBottom (12.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);

        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f));
        const auto nextName = cur + 1 < session.getNumScenes() ? "NEXT: " + session.getScene (cur + 1).name : juce::String ("END OF LIST");
        g.drawText (nextName, inner.withTrimmedTop (30.0f), juce::Justification::centredLeft, true);
    }

    // DSP per-core meters
    if (! cpuArea.isEmpty())
    {
        auto r = cpuArea;
        g.setFont (theme::font (10.5f, true));
        auto head = r.removeFromTop (14);
        const auto wg = session.getEngine().hasWorkgroup();
        const bool tight = head.getWidth() < 84;   // no room for the workgroup label: DSP in green = workgroup
        g.setColour (tight && wg ? theme::green : theme::textDim);
        g.drawText ("DSP " + juce::String (juce::roundToInt (totalLoad * 100.0f)) + "%", head, juce::Justification::centredLeft);

        g.setColour (wg ? theme::green : theme::textDim);
        const bool roomy = head.getWidth() >= 170;
        if (! tight) g.drawText (wg ? (WorkerPool::hasPerformanceLevels() ? (roomy ? "P-CORE WORKGROUP" : "P-CORE WG") : (roomy ? "AUDIO WORKGROUP" : "WG"))
                       : (roomy ? "NO WORKGROUP" : "NO WG"),
                    head, juce::Justification::centredRight);

        r.removeFromTop (3);
        auto status = r.removeFromBottom (13);
        auto bars = r.withTrimmedBottom (2);
        const int n = juce::jmax (1, (int) threadLoads.size());
        const float bw = (float) bars.getWidth() / (float) n;

        for (int i = 0; i < (int) threadLoads.size(); ++i)
        {
            auto b = juce::Rectangle<float> (bars.getX() + i * bw, (float) bars.getY(), bw - 3.0f, (float) bars.getHeight());
            g.setColour (theme::meterBg);
            g.fillRoundedRectangle (b, 2.0f);

            const auto l = threadLoads[(size_t) i];
            g.setColour (l > 0.85f ? theme::red : (l > 0.6f ? theme::amber : theme::accent));
            g.fillRoundedRectangle (b.withTrimmedTop (b.getHeight() * (1.0f - l)), 2.0f);
        }

        // single core / multicore: a small label over the top right of the bars (in single core
        // the worker bars there are empty anyway); multicore in the accent colour
        {
            const int cores = activeCores();
            const juce::String label = cores > 1 ? juce::String (cores) + " CORES" : juce::String ("1 CORE");
            const auto f = theme::font (9.0f, true);
            const auto w = juce::GlyphArrangement::getStringWidthInt (f, label) + 8;
            auto pill = juce::Rectangle<int> (bars.getRight() - w, bars.getY(), w, 12).toFloat();
            g.setColour (theme::bg.withAlpha (0.88f));
            g.fillRoundedRectangle (pill, 3.0f);
            g.setColour (cores > 1 ? theme::accent : theme::textDim);
            g.setFont (f);
            g.drawText (label, pill, juce::Justification::centred, false);
        }

        // dropouts since start / reset, then anything the Mac is doing wrong (low power, heat, memory)
        const auto drops = session.getEngine().dropouts.getTotal();
        g.setFont (theme::font (10.0f, drops > 0));
        juce::String text;

        if (const auto& b = session.getEngine().budget; b.isActive())
        {
            // the chains do not fit: say what to do, the count is in the tooltip
            g.setFont (theme::font (10.0f, true));
            g.setColour (theme::red);
            if (b.getRecommended() > 0)
                text << "BUFFER: USE " << b.getRecommended();   // short: the block is narrow
            else
                text << "OVER BUDGET";
        }
        else if (drops > 0)
        {
            g.setColour (theme::red);
            text << (int) drops << (drops == 1 ? " DROPOUT" : " DROPOUTS");
            if (const auto ago = secondsSinceLastDrop(); ago < 60.0)
                text << " \xc2\xb7 " << juce::roundToInt (ago) << " s ago";
        }
        else
        {
            g.setColour (theme::green);
            text << "0 dropouts";
        }

        if (systemProblems.isNotEmpty())
        {
            if (drops == 0)
                g.setColour (theme::amber);
            text << " \xc2\xb7 " << systemProblems;
        }

        g.drawText (text, status, juce::Justification::centredLeft, true);
    }

    // device info
    if (! deviceArea.isEmpty())
    {
        auto& dm = session.getEngine().getDeviceManager();
        juce::String line1 = "No audio device", line2;

        const auto problem = deviceStatus != nullptr ? deviceStatus() : juce::String();

        if (problem.isNotEmpty() && coreaudio::isReady())
        {
            g.setColour (theme::amber);
            g.setFont (theme::font (12.0f, true));
            g.drawText (problem, deviceArea.withHeight (18), juce::Justification::centredLeft, true);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.0f));
            g.drawText (problem.startsWith ("Waiting") ? "reconnects by itself, outputs off meanwhile" : "choose one in SETTINGS",
                        deviceArea.withTrimmedTop (18).withHeight (16), juce::Justification::centredLeft, true);
            return;
        }

        if (! coreaudio::isReady())
        {
            line1 = "Waiting for CoreAudio...";
            line2 = "coreaudiod is not responding";
        }
        else if (auto* dev = dm.getCurrentAudioDevice())
        {
            const auto sr = dev->getCurrentSampleRate();
            const auto bs = dev->getCurrentBufferSizeSamples();
            const auto lat = session.getEngine().deviceIoLatencySamples.load() + bs;   // no CoreAudio call while painting
            line1 = dev->getName();
            line2 = juce::String (sr / 1000.0, 1) + " kHz · " + juce::String (bs) + " smp · "
                  + juce::String (1000.0 * lat / sr, 1) + " ms RT";
        }

        g.setColour (theme::text);
        g.setFont (theme::font (12.0f, true));
        g.drawText (line1, deviceArea.withHeight (18), juce::Justification::centredLeft, true);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawText (line2, deviceArea.withTrimmedTop (18).withHeight (16), juce::Justification::centredLeft, true);
    }
}

void TopBar::collectMidiTargets (std::vector<MidiTargetHit>& hits)
{
    hits.push_back ({ masterMuteButton.getBounds(), MidiTarget::global (MidiTarget::masterMute) });
    hits.push_back ({ automixButton.getBounds(), MidiTarget::global (MidiTarget::automixOn) });
    hits.push_back ({ prevButton.getBounds(), MidiTarget::global (MidiTarget::scenePrev) });
    hits.push_back ({ nextButton.getBounds(), MidiTarget::global (MidiTarget::sceneNext) });

    for (auto* c : tempoCells)
        if (c->isVisible())
            hits.push_back ({ c->getBounds(), MidiTarget::global (MidiTarget::tapTempo, c->index) });
}

//==============================================================================
// Blocks of the top bar
juce::String TopBar::itemKey (int i)
{
    static const char* k[numItems] = { "session", "snapshots", "mute", "automix", "groups", "tempoWindow", "midi", "tempos",
                                       "dsp", "device", "pluginsSettings", "view", "layoutTheme" };
    return k[juce::jlimit (0, numItems - 1, i)];
}

juce::String TopBar::itemName (int i)
{
    static const char* n[numItems] = { "Session (name, New / Open / Save)", "Snapshots", "Mute all", "Automixer (on / meters)",
                                       "Edit groups", "Tempo window", "MIDI (map / window)", "Tap tempos",
                                       "DSP load and dropouts", "Audio device", "Plugins / Settings", "Rack view", "Layout / theme" };
    return n[juce::jlimit (0, numItems - 1, i)];
}

int TopBar::itemFeature (int i)
{
    switch (i)
    {
        case itSnapshots:   return features::snapshots;
        case itAutomix:     return features::automix;
        case itGroups:      return features::editGroups;
        case itTempoWindow:
        case itTempos:      return features::tempo;
        case itMidi:        return features::midi;
        default:            return -1;
    }
}

static int sectionOf (int i)
{
    switch (i)
    {
        case TopBar::itSession:   return 0;
        case TopBar::itSnapshots: return 1;
        case TopBar::itTempos:    return 3;
        case TopBar::itDsp: case TopBar::itDevice: case TopBar::itPluginsSettings: return 4;
        case TopBar::itView: case TopBar::itLayoutTheme: return 5;
        default:                  return 2;   // live: mute, automixer, groups, tempo, MIDI
    }
}

/** One-button blocks: two of them side by side share a column (one above the other). */
static bool isHalf (int i)   { return i == TopBar::itGroups || i == TopBar::itTempoWindow; }

juce::String TopBar::defaultLayoutString()
{
    juce::StringArray keys;
    for (int i = 0; i < numItems; ++i)
        keys.add (itemKey (i));
    return keys.joinIntoString (",");
}

void TopBar::setLayoutString (const juce::String& layout)
{
    order.clear();
    hidden.fill (false);

    for (auto token : juce::StringArray::fromTokens (layout, ",", {}))
    {
        token = token.trim();
        const bool hide = token.startsWithChar ('-');
        const auto key = hide ? token.substring (1) : token;

        for (int i = 0; i < numItems; ++i)
            if (itemKey (i) == key && std::find (order.begin(), order.end(), i) == order.end())
            {
                order.push_back (i);
                hidden[(size_t) i] = hide;
            }
    }

    // blocks added in a later version: in their default place (after the block before them)
    for (int i = 0; i < numItems; ++i)
        if (std::find (order.begin(), order.end(), i) == order.end())
        {
            auto at = order.end();
            if (i > 0)
                if (auto prev = std::find (order.begin(), order.end(), i - 1); prev != order.end())
                    at = prev + 1;
            order.insert (at, i);
        }

    resized();
    repaint();
}

juce::String TopBar::getLayoutString() const
{
    juce::StringArray keys;
    for (auto i : order)
        keys.add ((hidden[(size_t) i] ? "-" : "") + itemKey (i));
    return keys.joinIntoString (",");
}

bool TopBar::isItemShown (int i) const
{
    const auto f = itemFeature (i);
    return ! hidden[(size_t) i] && (f < 0 || features::has ((features::Feature) f));
}

void TopBar::setItemHidden (int i, bool h)
{
    if (hidden[(size_t) i] == h)
        return;

    hidden[(size_t) i] = h;
    itemsChanged();
}

void TopBar::moveItem (int item, int newIndex)
{
    auto it = std::find (order.begin(), order.end(), item);
    if (it == order.end())
        return;

    order.erase (it);
    newIndex = juce::jlimit (0, (int) order.size(), newIndex);
    order.insert (order.begin() + newIndex, item);
    itemsChanged();
}

void TopBar::featuresChanged()
{
    resized();
    repaint();
    if (customizeWindow != nullptr)
        if (auto* c = customizeWindow->getContentComponent())
            c->resized();
}

void TopBar::itemsChanged()
{
    featuresChanged();
    if (onLayoutChanged) onLayoutChanged (getLayoutString());
}

std::vector<juce::Component*> TopBar::componentsOf (int item)
{
    switch (item)
    {
        case itSession:         return { &newButton, &openButton, &saveButton };
        case itSnapshots:       return { &prevButton, &nextButton, &storeButton };
        case itMute:            return { &masterMuteButton };
        case itAutomix:         return { &automixButton, &automixViewButton };
        case itGroups:          return { &groupsButton };
        case itTempoWindow:     return { &tempoButton };
        case itMidi:            return { &midiMapButton, &midiButton };
        case itTempos:
        {
            std::vector<juce::Component*> v { &addTempoButton };
            for (auto* c : tempoCells) v.push_back (c);
            return v;
        }
        case itPluginsSettings: return { &pluginsButton, &settingsButton };
        case itView:            return { &viewButton };
        case itLayoutTheme:     return { &layoutButton, &themeButton };
        default:                return {};
    }
}

//==============================================================================
void TopBar::resized()
{
    auto r = getLocalBounds().reduced (10, 10);
    separators.clearQuick();
    const int rowH = (r.getHeight() - 4) / 2;

    logoArea = sceneArea = cpuArea = deviceArea = {};
    for (auto& a : itemArea) a = {};

    for (int i = 0; i < numItems; ++i)
        if (! isItemShown (i))
            for (auto* c : componentsOf (i))
                c->setVisible (false);

    // blocks in the user's order; two one-button blocks next to each other share a column
    struct Block { int items[2] { -1, -1 }; int n = 0; int w = 0, floor = 0, maxExtra = 0; };
    std::vector<Block> blocks;

    for (auto i : order)
    {
        if (! isItemShown (i))
            continue;

        if (isHalf (i) && ! blocks.empty() && blocks.back().n == 1 && isHalf (blocks.back().items[0]))
        {
            blocks.back().items[1] = i;
            blocks.back().n = 2;
            continue;
        }

        Block b;
        b.items[0] = i;
        b.n = 1;
        blocks.push_back (b);
    }

    if (blocks.empty())
        return;

    // tap tempos: one cell per tempo + "+"; all of them must be there if there is room
    const int cellMax = 52, cellMin = 42, plusW = 26;
    const int n = juce::jmax (1, tempoCells.size());
    auto tempoWidth = [n, plusW] (int cell) { return n * cell + (n - 1) * 4 + 4 + plusW; };
    const int tempoMin = tempoWidth (cellMax) - (n - 1) * (cellMax + 4);   // one full cell + "+"
    Block* tempoBlock = nullptr;
    Block* sceneBlock = nullptr;
    Block* sessionBlock = nullptr;
    Block* cpuBlock = nullptr;
    Block* deviceBlock = nullptr;

    for (auto& b : blocks)
    {
        switch (b.items[0])
        {
            case itSession:         b.w = 172; b.floor = 172; b.maxExtra = 50;  sessionBlock = &b; break;
            case itSnapshots:       b.w = 222; b.floor = 170; b.maxExtra = 330; sceneBlock = &b; break;
            case itMute:            b.w = 62; break;
            case itAutomix:         b.w = 78; break;
            case itGroups:
            case itTempoWindow:     b.w = 64; break;
            case itMidi:            b.w = 64; break;
            case itTempos:          b.w = tempoMin; tempoBlock = &b; break;
            case itDsp:             b.w = 64; b.floor = 52; b.maxExtra = 140; cpuBlock = &b; break;
            case itDevice:          b.w = 80; b.floor = 56; b.maxExtra = 100000; deviceBlock = &b; break;
            case itPluginsSettings: b.w = 80; break;
            case itView:            b.w = 62; break;
            case itLayoutTheme:     b.w = 80; break;
            default: break;
        }
        if (b.floor == 0) b.floor = b.w;
    }

    std::vector<int> gaps (blocks.size(), 0);   // gap before block k
    int sumGaps = 0, sumW = 0;
    for (size_t k = 0; k < blocks.size(); ++k)
    {
        if (k > 0)
            gaps[k] = sectionOf (blocks[k].items[0]) != sectionOf (blocks[k - 1].items[0]) ? 12 : 4;
        sumGaps += gaps[k];
        sumW += blocks[k].w;
    }

    int extra = r.getWidth() - sumGaps - sumW;

    // too narrow: take width from the flexible blocks (snapshot display, device, DSP) down to their floor
    auto takeFloors = [&] (int need)
    {
        for (auto* b : { sceneBlock, deviceBlock, cpuBlock })
            if (b != nullptr && need > 0)
            {
                const int steal = juce::jlimit (0, b->w - b->floor, need);
                b->w -= steal;
                need -= steal;
            }
        return need;
    };

    int cellW = cellMax;

    if (tempoBlock != nullptr)
    {
        const int want = tempoWidth (cellMax) - tempoMin;

        if (want <= extra)
        {
            tempoBlock->w = tempoWidth (cellMax);
            extra -= want;
        }
        else
        {
            // not enough room: narrower cells, then narrower flexible blocks
            int avail = tempoMin + juce::jmax (0, extra);
            cellW = (avail - 4 - plusW - (n - 1) * 4) / n;

            if (cellW < cellMin)
            {
                const int missing = tempoWidth (cellMin) - avail;
                avail += missing - takeFloors (missing);
                cellW = juce::jmax (34, (avail - 4 - plusW - (n - 1) * 4) / n);
            }

            tempoBlock->w = juce::jmin (avail, tempoWidth (cellW));
            extra = juce::jmin (0, extra);
        }
    }

    if (extra < 0)
        extra = -takeFloors (-extra);

    // room to spare: the snapshot display, the session name, the DSP meter, then the device info
    if (extra > 0)
    {
        auto give = [&extra] (Block* b, int amount)
        {
            if (b == nullptr) return;
            amount = juce::jlimit (0, juce::jmin (b->maxExtra, extra), amount);
            b->w += amount;
            extra -= amount;
        };
        give (sceneBlock, extra / 2);
        give (sessionBlock, extra / 4);
        give (cpuBlock, extra / 3);
        give (deviceBlock, extra);
    }

    // anything left (device hidden): the last section stays on the right edge
    if (extra > 0)
    {
        size_t k = blocks.size() - 1;
        while (k > 0 && gaps[k] == 4)
            --k;
        if (k > 0)
            gaps[k] += extra;
    }

    auto stacked = [rowH] (juce::Rectangle<int> col, juce::Component& top, juce::Component& bottom)
    {
        top.setBounds (col.removeFromTop (rowH));
        bottom.setBounds (col.removeFromBottom (rowH));
    };

    int x = r.getX();

    for (size_t k = 0; k < blocks.size(); ++k)
    {
        if (gaps[k] > 4)
            separators.add (x + gaps[k] - 6);   // in the middle of the 12 px after any extra space

        x += gaps[k];
        auto& b = blocks[k];
        auto a = juce::Rectangle<int> (x, r.getY(), juce::jmax (0, b.w), r.getHeight());
        x += b.w;

        if (b.n == 2)
        {
            itemArea[(size_t) b.items[0]] = a.withHeight (rowH);
            itemArea[(size_t) b.items[1]] = a.withTrimmedTop (a.getHeight() - rowH);
        }
        else
        {
            itemArea[(size_t) b.items[0]] = a;
        }

        for (int h = 0; h < b.n; ++h)
        {
            const int it = b.items[h];
            for (auto* c : componentsOf (it))
                if (c != nullptr && dynamic_cast<TempoCell*> (c) == nullptr)
                    c->setVisible (true);

            auto ia = itemArea[(size_t) it];

            switch (it)
            {
                case itSession:
                {
                    logoArea = ia.removeFromTop (rowH);
                    auto row = ia.removeFromBottom (rowH);
                    const int w3 = (row.getWidth() - 8) / 3;
                    newButton.setBounds (row.removeFromLeft (w3));
                    row.removeFromLeft (4);
                    openButton.setBounds (row.removeFromLeft (w3));
                    row.removeFromLeft (4);
                    saveButton.setBounds (row);
                    break;
                }
                case itSnapshots:
                    prevButton.setBounds (ia.removeFromLeft (30).reduced (0, 6));
                    ia.removeFromLeft (4);
                    storeButton.setBounds (ia.removeFromRight (58).reduced (0, 6));
                    ia.removeFromRight (4);
                    nextButton.setBounds (ia.removeFromRight (30).reduced (0, 6));
                    ia.removeFromRight (4);
                    sceneArea = ia;
                    break;
                case itMute:            masterMuteButton.setBounds (ia); break;
                case itAutomix:         stacked (ia, automixButton, automixViewButton); break;
                case itGroups:          groupsButton.setBounds (b.n == 2 ? ia : ia.withSizeKeepingCentre (ia.getWidth(), rowH)); break;
                case itTempoWindow:     tempoButton.setBounds (b.n == 2 ? ia : ia.withSizeKeepingCentre (ia.getWidth(), rowH)); break;
                case itMidi:            stacked (ia, midiMapButton, midiButton); break;
                case itTempos:
                {
                    auto cells = ia.withTrimmedRight (plusW + 4);

                    for (auto* c : tempoCells)
                    {
                        const bool fits = cells.getWidth() >= cellW;
                        c->setVisible (fits);
                        if (fits) { c->setBounds (cells.removeFromLeft (cellW).reduced (0, 7)); cells.removeFromLeft (4); }
                    }

                    // "+" right after the last cell shown
                    addTempoButton.setBounds (juce::Rectangle<int> (cells.getX(), ia.getY(), plusW, ia.getHeight()).reduced (0, 14));
                    break;
                }
                case itDsp:             cpuArea = ia; break;
                case itDevice:          deviceArea = ia.withSizeKeepingCentre (ia.getWidth(), 36); break;
                case itPluginsSettings: stacked (ia, pluginsButton, settingsButton); break;
                case itView:            viewButton.setBounds (ia.reduced (0, 6)); break;
                case itLayoutTheme:     stacked (ia, layoutButton, themeButton); break;
                default: break;
            }
        }
    }
}

//==============================================================================
// Edit mode: drag the blocks
void TopBar::setCustomizing (bool on)
{
    customizing = on;
    dragItem = dropIndex = -1;

    for (auto* c : getChildren())
        c->setInterceptsMouseClicks (! on, ! on);

    repaint();
}

int TopBar::dropIndexAt (int x) const
{
    for (size_t k = 0; k < order.size(); ++k)
        if (isItemShown (order[k]) && x < itemArea[(size_t) order[k]].getCentreX())
            return (int) k;

    return (int) order.size();
}

void TopBar::mouseDrag (const juce::MouseEvent& e)
{
    if (! customizing || dragItem < 0 || e.getDistanceFromDragStart() < 4)
        return;

    dropIndex = dropIndexAt (e.getEventRelativeTo (this).x);
    repaint();
}

void TopBar::mouseUp (const juce::MouseEvent&)
{
    if (customizing && dragItem >= 0 && dropIndex >= 0)
    {
        const auto from = (int) (std::find (order.begin(), order.end(), dragItem) - order.begin());
        moveItem (dragItem, dropIndex > from ? dropIndex - 1 : dropIndex);
    }

    dragItem = dropIndex = -1;
    repaint();
}

void TopBar::paintOverChildren (juce::Graphics& g)
{
    if (! customizing)
        return;

    g.setColour (theme::panel.withAlpha (0.35f));
    g.fillRect (getLocalBounds());

    for (int i = 0; i < numItems; ++i)
    {
        if (! isItemShown (i) || itemArea[(size_t) i].isEmpty())
            continue;

        const auto a = itemArea[(size_t) i].toFloat().expanded (2.0f, 2.0f);
        g.setColour (theme::accent.withAlpha (i == dragItem ? 0.35f : 0.1f));
        g.fillRoundedRectangle (a, 5.0f);
        g.setColour (theme::accent.withAlpha (0.8f));
        g.drawRoundedRectangle (a.reduced (0.5f), 5.0f, 1.0f);
    }

    if (dropIndex >= 0)
    {
        int x = getWidth() - 6;
        for (size_t k = (size_t) dropIndex; k < order.size(); ++k)
            if (isItemShown (order[k])) { x = itemArea[(size_t) order[k]].getX() - 5; break; }

        g.setColour (theme::accent);
        g.fillRoundedRectangle ((float) x - 1.5f, 4.0f, 3.0f, (float) getHeight() - 8.0f, 1.5f);
    }
}

//==============================================================================
/** The Customize Top Bar window: show / hide every block, move it earlier / later, reset. While
    it is open the top bar is in edit mode (drag the blocks there). */
class TopBarCustomizer final : public juce::Component
{
public:
    explicit TopBarCustomizer (TopBar& b) : bar (b)
    {
        info.setText (juce::String::fromUTF8 ("Drag the blocks in the top bar to move them (or use \xe2\x97\x80 \xe2\x96\xb6 here), "
                                              "untick to hide. Functions switched off in SETTINGS \xe2\x80\xba Features never show."),
                      juce::dontSendNotification);
        info.setFont (theme::font (12.0f));
        info.setColour (juce::Label::textColourId, theme::textDim);
        info.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (info);

        reset.setButtonText ("Reset");
        reset.onClick = [this] { juce::MessageManager::callAsync ([&b = bar] { b.setLayoutString (TopBar::defaultLayoutString()); b.itemsChanged(); }); };
        addAndMakeVisible (reset);
        done.setButtonText ("Done");
        done.onClick = [this]
        {
            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                w->closeButtonPressed();
        };
        addAndMakeVisible (done);
        setSize (420, 110 + TopBar::numItems * 30);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::bg); }

    void resized() override
    {
        rows.clear();
        auto r = getLocalBounds().reduced (12);
        info.setBounds (r.removeFromTop (36));
        r.removeFromTop (6);
        auto bottom = r.removeFromBottom (28);
        done.setBounds (bottom.removeFromRight (90));
        bottom.removeFromRight (8);
        reset.setBounds (bottom.removeFromRight (90));

        const auto& order = bar.getOrder();

        for (size_t k = 0; k < order.size(); ++k)
        {
            const int it = order[k];
            auto* row = rows.add (new Row (bar, it, (int) k, (int) order.size()));
            addAndMakeVisible (row);
            row->setBounds (r.removeFromTop (28));
            r.removeFromTop (2);
        }
    }

private:
    struct Row final : juce::Component
    {
        Row (TopBar& b, int item, int index, int count) : bar (b), it (item), k (index)
        {
            const auto f = TopBar::itemFeature (it);
            const bool featureOn = f < 0 || features::has ((features::Feature) f);
            toggle.setButtonText (TopBar::itemName (it) + (featureOn ? juce::String() : "  (off in Features)"));
            toggle.setToggleState (! bar.isItemHidden (it), juce::dontSendNotification);
            toggle.setEnabled (featureOn);
            toggle.onClick = [this]
            {
                const bool show = toggle.getToggleState();
                juce::MessageManager::callAsync ([&b = bar, i = it, show] { b.setItemHidden (i, ! show); });
            };
            addAndMakeVisible (toggle);

            left.setButtonText (juce::String::fromUTF8 ("\xe2\x97\x80"));
            right.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xb6"));
            left.setEnabled (k > 0);
            right.setEnabled (k < count - 1);
            left.setTooltip ("Earlier (more to the left)");
            right.setTooltip ("Later (more to the right)");
            left.onClick = [this]  { juce::MessageManager::callAsync ([&b = bar, i = it, to = k - 1] { b.moveItem (i, to); }); };
            right.onClick = [this] { juce::MessageManager::callAsync ([&b = bar, i = it, to = k + 1] { b.moveItem (i, to); }); };
            addAndMakeVisible (left);
            addAndMakeVisible (right);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            right.setBounds (r.removeFromRight (34));
            r.removeFromRight (4);
            left.setBounds (r.removeFromRight (34));
            toggle.setBounds (r);
        }

        TopBar& bar;
        int it, k;
        juce::ToggleButton toggle;
        juce::TextButton left, right;
    };

    TopBar& bar;
    juce::Label info;
    juce::TextButton reset, done;
    juce::OwnedArray<Row> rows;
};

void TopBar::showCustomizeWindow()
{
    struct Window final : juce::DocumentWindow
    {
        Window (TopBar& b) : juce::DocumentWindow ("Customize Top Bar", theme::bg, juce::DocumentWindow::closeButton), bar (b)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new TopBarCustomizer (b), true);
            setAlwaysOnTop (true);
        }
        void closeButtonPressed() override
        {
            setVisible (false);
            bar.setCustomizing (false);
        }
        TopBar& bar;
    };

    if (customizeWindow == nullptr)
    {
        customizeWindow = std::make_unique<Window> (*this);
        const auto topLeft = localPointToGlobal (juce::Point<int> (getWidth() / 2 - 210, getHeight() + 20));
        customizeWindow->setTopLeftPosition (topLeft);
    }

    setCustomizing (true);
    customizeWindow->setVisible (true);
    customizeWindow->toFront (true);
}

} // namespace stagerack
