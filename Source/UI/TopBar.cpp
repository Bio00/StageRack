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
    prevButton.setTooltip ("Previous scene (Left arrow)");
    nextButton.setTooltip ("Next scene (Right arrow / Space)");

    storeButton.setTooltip ("Store current settings into the active scene (Cmd+Shift+S)");
    storeButton.onClick = [this]
    {
        if (session.getCurrentScene() >= 0)
            session.storeScene (session.getCurrentScene());
        else
            session.addScene ("Scene 1");
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
    automixButton.setTooltip ("Gain-sharing automixer on/off for all groups (racks join a group from the rack menu > Automix). Stored in scenes.");
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
    if (! cpuArea.contains (e.getPosition()))
        return;

    juce::PopupMenu m;
    m.addItem ("Reset dropout counter", [safe = juce::Component::SafePointer<TopBar> (this)]
    {
        if (safe != nullptr) { safe->session.getEngine().dropouts.clearCounters(); safe->repaint (safe->cpuArea); }
    });
    const auto log = PluginManager::getDataDirectory().getChildFile ("dropouts.log");
    m.addItem ("Show dropout log", log.existsAsFile(), false, [log] { log.revealToUser(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (cpuArea)));
}

void TopBar::mouseMove (const juce::MouseEvent& e)
{
    juce::String tip;

    if (cpuArea.contains (e.getPosition()))
        tip = dropoutTooltip();

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
            << '|' << (session.getEngine().hasWorkgroup() ? 1 : 0);

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
    {
        auto r = sceneArea.toFloat();
        g.setColour (theme::display);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (theme::accent.withAlpha (0.6f));
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

        const auto cur = session.getCurrentScene();
        const auto num = cur >= 0 ? juce::String (cur + 1).paddedLeft ('0', 2) : juce::String ("--");
        const auto name = cur >= 0 ? session.getScene (cur).name : juce::String ("No scene");

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

        // dropouts since start / reset, then anything the Mac is doing wrong (low power, heat, memory)
        const auto drops = session.getEngine().dropouts.getTotal();
        g.setFont (theme::font (10.0f, drops > 0));
        juce::String text;

        if (drops > 0)
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

void TopBar::resized()
{
    auto r = getLocalBounds().reduced (10, 10);
    separators.clearQuick();
    const int gap = 12, rowH = (r.getHeight() - 4) / 2;

    // minimum widths of the flexible parts; what is left goes first to the tap tempos (all of them
    // must be there), then to the scene, device and DSP areas
    const int sessionMin = 172;
    int cpuMin = 64, deviceMin = 80;
    int sceneMin = 222;
    const int liveW = 62 + 4 + 78 + 4 + 64 + 4 + 64, engineButtonsW = 80, viewW = 80 + 4 + 62;
    const int cellMax = 52, cellMin = 42, plusW = 26;
    const int n = juce::jmax (1, tempoCells.size());
    auto tempoWidth = [n, plusW] (int cell) { return n * cell + (n - 1) * 4 + 4 + plusW; };
    const int tempoMin = tempoWidth (cellMax) - (n - 1) * (cellMax + 4);   // one full cell + "+"
    const int fixed = sessionMin + liveW + tempoMin + engineButtonsW + viewW + 5 * gap + 2 * 6 + sceneMin + cpuMin + deviceMin;
    int extra = juce::jmax (0, r.getWidth() - fixed);

    int cellW = cellMax;
    int tempoW = tempoWidth (cellMax);

    if (tempoW - tempoMin > extra)
    {
        // not enough room: narrower cells, then a narrower scene display
        int avail = tempoMin + extra;
        cellW = (avail - 4 - plusW - (n - 1) * 4) / n;

        if (cellW < cellMin)
        {
            // take the missing width from the scene display, then the device info, then the DSP meter
            auto take = [&avail, need = tempoWidth (cellMin)] (int& w, int floor)
            {
                const int steal = juce::jlimit (0, juce::jmax (0, w - floor), need - avail);
                w -= steal;
                avail += steal;
            };
            take (sceneMin, 170);
            take (deviceMin, 56);
            take (cpuMin, 52);
            cellW = juce::jmax (34, (avail - 4 - plusW - (n - 1) * 4) / n);
        }

        tempoW = juce::jmin (avail, tempoWidth (cellW));
        extra = 0;
    }
    else
    {
        extra -= tempoW - tempoMin;
    }

    const int sceneExtra   = juce::jmin (330, extra / 2);
    const int sessionExtra = juce::jmin (50, (extra - sceneExtra) / 4);
    const int cpuExtra     = juce::jmin (140, (extra - sceneExtra - sessionExtra) / 3);   // the rest goes to the device info

    auto separator = [this, &r, gap]
    {
        separators.add (r.getX() + gap / 2);
        r.removeFromLeft (gap);
    };

    auto stacked = [rowH] (juce::Rectangle<int> col, juce::Button& top, juce::Button& bottom)
    {
        top.setBounds (col.removeFromTop (rowH));
        bottom.setBounds (col.removeFromBottom (rowH));
    };

    // --- session: logo + file name, NEW / OPEN / SAVE
    {
        auto sec = r.removeFromLeft (sessionMin + sessionExtra);
        logoArea = sec.removeFromTop (rowH);
        auto row = sec.removeFromBottom (rowH);
        const int w3 = (row.getWidth() - 8) / 3;
        newButton.setBounds (row.removeFromLeft (w3));
        row.removeFromLeft (4);
        openButton.setBounds (row.removeFromLeft (w3));
        row.removeFromLeft (4);
        saveButton.setBounds (row);
    }
    separator();

    // --- scenes
    {
        auto scene = r.removeFromLeft (sceneMin + sceneExtra);
        prevButton.setBounds (scene.removeFromLeft (30).reduced (0, 6));
        scene.removeFromLeft (4);
        storeButton.setBounds (scene.removeFromRight (58).reduced (0, 6));
        scene.removeFromRight (4);
        nextButton.setBounds (scene.removeFromRight (30).reduced (0, 6));
        scene.removeFromRight (4);
        sceneArea = scene;
    }
    separator();

    // --- live: master mute, automixer, edit groups / tempo windows
    {
        auto sec = r.removeFromLeft (liveW);
        masterMuteButton.setBounds (sec.removeFromLeft (62));
        sec.removeFromLeft (4);
        stacked (sec.removeFromLeft (78), automixButton, automixViewButton);
        sec.removeFromLeft (4);
        stacked (sec.removeFromLeft (64), groupsButton, tempoButton);
        sec.removeFromLeft (4);
        stacked (sec, midiMapButton, midiButton);
    }
    separator();

    // --- tap tempos: one cell per tempo (BPM + TAP), then "+"
    {
        auto sec = r.removeFromLeft (tempoW);
        auto cells = sec.withTrimmedRight (plusW + 4);

        for (auto* c : tempoCells)
        {
            const bool fits = cells.getWidth() >= cellW;
            c->setVisible (fits);
            if (fits) { c->setBounds (cells.removeFromLeft (cellW).reduced (0, 7)); cells.removeFromLeft (4); }
        }

        // "+" right after the last cell shown
        addTempoButton.setBounds (juce::Rectangle<int> (cells.getX(), sec.getY(), plusW, sec.getHeight()).reduced (0, 14));
    }
    separator();

    // --- view (rightmost), then the audio engine in between
    {
        auto view = r.removeFromRight (viewW);
        viewButton.setBounds (view.removeFromLeft (62).reduced (0, 6));
        view.removeFromLeft (4);
        stacked (view, layoutButton, themeButton);
        separators.add (r.getRight() - gap / 2);
        r.removeFromRight (gap);
    }

    stacked (r.removeFromRight (engineButtonsW), pluginsButton, settingsButton);
    r.removeFromRight (gap);
    cpuArea = r.removeFromLeft (cpuMin + cpuExtra);
    r.removeFromLeft (gap);
    deviceArea = r.withSizeKeepingCentre (r.getWidth(), 36);
}

} // namespace stagerack
