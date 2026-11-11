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

    for (auto* b : { &prevButton, &nextButton, &storeButton, &newButton, &openButton, &saveButton, &pluginsButton, &settingsButton, &layoutButton })
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

    automixViewButton.setTooltip ("Automixer window: input level and automix gain of every mic, weight, group");
    automixViewButton.onClick = [this] { if (onAutomixView) onAutomixView(); };
    addAndMakeVisible (automixViewButton);
}

void TopBar::lookAndFeelChanged()
{
    storeButton.setColour (juce::TextButton::buttonColourId, theme::accent.withAlpha (0.25f));
    automixButton.setColour (juce::TextButton::buttonOnColourId, theme::green.withAlpha (0.8f));
}

void TopBar::setVerticalLayout (bool v)
{
    layoutButton.setButtonText (v ? "HORIZONTAL" : "VERTICAL");
}

void TopBar::refresh()
{
    storeButton.setColour (juce::TextButton::buttonColourId, theme::accent.withAlpha (0.25f));
    repaint();
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

    repaint (cpuArea);
    repaint (deviceArea);
}

void TopBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

    // logo
    g.setColour (theme::text);
    g.setFont (theme::font (19.0f, true));
    g.drawText ("STAGE", 16, 14, 80, 24, juce::Justification::centredLeft);
    g.setColour (theme::accent);
    g.drawText ("RACK", 82, 14, 70, 24, juce::Justification::centredLeft);

    g.setColour (theme::textDim);
    g.setFont (theme::font (10.5f));
    auto f = session.getFile();
    g.drawText ((f == juce::File() ? juce::String ("Untitled") : f.getFileNameWithoutExtension()) + (session.isDirty() ? " *" : ""),
                16, 40, 150, 16, juce::Justification::centredLeft, true);

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

        g.setColour (theme::accent);
        g.setFont (theme::font (30.0f, true));
        g.drawText (num, r.reduced (12, 0).removeFromLeft (56), juce::Justification::centredLeft);

        g.setColour (theme::text);
        g.setFont (theme::font (19.0f, true));
        g.drawText (name, r.reduced (12, 0).withTrimmedLeft (60).withTrimmedBottom (12), juce::Justification::centredLeft, true);

        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f));
        const auto nextName = cur + 1 < session.getNumScenes() ? "NEXT: " + session.getScene (cur + 1).name : juce::String ("END OF LIST");
        g.drawText (nextName, r.reduced (12, 0).withTrimmedLeft (60).withTrimmedTop (30), juce::Justification::centredLeft, true);
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
        g.drawText (wg ? (WorkerPool::hasPerformanceLevels() ? "P-CORE WORKGROUP" : "AUDIO WORKGROUP") : "NO WORKGROUP", head, juce::Justification::centredRight);

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
    r.removeFromLeft (160);

    // scene section
    auto scene = r.removeFromLeft (juce::jlimit (380, 440, r.getWidth() / 3));
    prevButton.setBounds (scene.removeFromLeft (38).reduced (0, 6));
    scene.removeFromLeft (6);
    storeButton.setBounds (scene.removeFromRight (64).reduced (0, 6));
    scene.removeFromRight (6);
    nextButton.setBounds (scene.removeFromRight (38).reduced (0, 6));
    scene.removeFromRight (6);
    sceneArea = scene;

    r.removeFromLeft (16);

    // right: buttons
    auto buttons = r.removeFromRight (330);
    auto top = buttons.removeFromTop (buttons.getHeight() / 2).reduced (0, 2);
    auto bottom = buttons.reduced (0, 2);
    const int w3 = top.getWidth() / 3;
    newButton.setBounds (top.removeFromLeft (w3).reduced (2, 0));
    openButton.setBounds (top.removeFromLeft (w3).reduced (2, 0));
    saveButton.setBounds (top.reduced (2, 0));
    pluginsButton.setBounds (bottom.removeFromLeft (w3).reduced (2, 0));
    settingsButton.setBounds (bottom.removeFromLeft (w3).reduced (2, 0));
    layoutButton.setBounds (bottom.reduced (2, 0));

    r.removeFromRight (8);
    {
        auto am = r.removeFromRight (84);
        automixButton.setBounds (am.removeFromTop (am.getHeight() / 2).reduced (0, 2));
        automixViewButton.setBounds (am.reduced (0, 2));
    }
    r.removeFromRight (12);

    cpuArea = r.removeFromLeft (juce::jmin (220, r.getWidth() / 2));
    r.removeFromLeft (12);
    deviceArea = r.withSizeKeepingCentre (r.getWidth(), 36);
}

} // namespace stagerack
