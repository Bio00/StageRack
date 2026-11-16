#include "TopBar.h"
#include "../Engine/CoreAudioProbe.h"
#include "Theme.h"

namespace stagerack
{

TopBar::TopBar (Session& s) : session (s)
{
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

    for (auto* b : { &prevButton, &nextButton, &storeButton, &newButton, &openButton, &saveButton, &pluginsButton, &settingsButton, &layoutButton, &themeButton })
        addAndMakeVisible (*b);

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

juce::Rectangle<float> TopBar::groupPill (int group) const
{
    // 4 x 2 grid, A-D on top
    const auto a = groupsArea.toFloat();
    const float w = (a.getWidth() - 3 * 3.0f) / 4.0f, h = (a.getHeight() - 4.0f) / 2.0f;
    const int i = group - 1;
    return { a.getX() + (float) (i % 4) * (w + 3.0f), a.getY() + (float) (i / 4) * (h + 4.0f), w, h };
}

void TopBar::mouseDown (const juce::MouseEvent& e)
{
    // edit groups: click = suspend / resume
    for (int g = 1; g <= Rack::maxEditGroups; ++g)
        if (groupPill (g).contains (e.position) && session.numRacksInEditGroup (g) > 0)
        {
            session.setEditGroupActive (g, ! session.isEditGroupActive (g));
            if (auto* p = getParentComponent()) p->repaint();
            return;
        }
}

void TopBar::mouseMove (const juce::MouseEvent& e)
{
    juce::String tip;

    if (groupsArea.contains (e.getPosition()))
        tip = "Edit groups (Pro Tools style): racks of an active group follow each other's gain, mute/solo/bypass and plugins. "
              "Click a letter to suspend / resume it, Cmd+Shift+G = all. Racks join a group from the rack menu > Edit group.";
    else if (logoArea.contains (e.getPosition()))
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

    repaint (cpuArea);
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

    // edit groups A-H: filled = active, outlined = suspended, faint = unused
    for (int grp = 1; grp <= Rack::maxEditGroups && ! groupsArea.isEmpty(); ++grp)
    {
        const auto pill = groupPill (grp);
        const auto c = theme::groupColour (grp);
        const auto used = session.numRacksInEditGroup (grp) > 0;
        const auto on = used && session.isEditGroupActive (grp);

        g.setColour (on ? c : (used ? c.withAlpha (0.12f) : theme::meterBg));
        g.fillRoundedRectangle (pill, 3.0f);
        g.setColour (used ? c : theme::outline);
        if (! on) g.drawRoundedRectangle (pill.reduced (0.5f), 3.0f, 1.0f);
        g.setColour (on ? juce::Colour (0xff101214) : (used ? c : theme::textDim.withAlpha (0.5f)));
        g.setFont (theme::font (10.5f, true));
        g.drawText (theme::groupLetter (grp), pill, juce::Justification::centred, false);
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
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f, true));
        auto head = r.removeFromTop (14);
        g.drawText ("DSP " + juce::String (juce::roundToInt (totalLoad * 100.0f)) + "%", head, juce::Justification::centredLeft);

        const auto wg = session.getEngine().hasWorkgroup();
        g.setColour (wg ? theme::green : theme::textDim);
        const bool roomy = head.getWidth() >= 170;
        g.drawText (wg ? (WorkerPool::hasPerformanceLevels() ? (roomy ? "P-CORE WORKGROUP" : "P-CORE WG") : (roomy ? "AUDIO WORKGROUP" : "WG"))
                       : (roomy ? "NO WORKGROUP" : "NO WG"),
                    head, juce::Justification::centredRight);

        r.removeFromTop (4);
        auto bars = r.removeFromTop (34);
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

        g.setColour (theme::textDim);
        g.setFont (theme::font (9.5f));
        g.drawText (juce::String ((int) threadLoads.size()) + " threads", r, juce::Justification::centredLeft);
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
            const auto lat = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples() + bs;
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

void TopBar::resized()
{
    auto r = getLocalBounds().reduced (10, 10);
    separators.clearQuick();
    const int gap = 12, rowH = (r.getHeight() - 4) / 2;

    // minimum widths of the flexible parts; what is left goes to the scene, device and DSP areas
    const int sessionMin = 172, sceneMin = 270, cpuMin = 72, deviceMin = 92;
    const int liveW = 68 + 4 + 84 + 6 + 78, engineButtonsW = 80, viewW = 80;
    const int fixed = sessionMin + liveW + engineButtonsW + viewW + 4 * gap + 2 * 6 + sceneMin + cpuMin + deviceMin;
    const int extra = juce::jmax (0, r.getWidth() - fixed);

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

    // --- live: master mute, automixer, edit groups
    {
        auto sec = r.removeFromLeft (liveW);
        masterMuteButton.setBounds (sec.removeFromLeft (68));
        sec.removeFromLeft (4);
        stacked (sec.removeFromLeft (84), automixButton, automixViewButton);
        sec.removeFromLeft (6);
        groupsArea = sec;
    }
    separator();

    // --- view (rightmost), then the audio engine in between
    {
        auto view = r.removeFromRight (viewW);
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
