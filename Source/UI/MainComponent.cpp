#include "MainComponent.h"
#include "TopBar.h"
#include "SceneListComponent.h"
#include "RackComponent.h"
#include "PluginWindow.h"
#include "PluginManagerWindow.h"

namespace stagerack
{

MainComponent::MainComponent()
{
    auto& settings = pluginManager.getSettings();
    theme::setLight (settings.getBoolValue ("lightTheme", false));
    lnf.applyColours();
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

    engine.nextSceneCC = settings.getIntValue ("nextSceneCC", 80);
    engine.prevSceneCC = settings.getIntValue ("prevSceneCC", 81);
    Rack::asyncSafetyMs = (float) settings.getDoubleValue ("asyncSafetyMs", 3.0);
    engine.hopAlignEnabled = settings.getBoolValue ("hopAlign", true);
    engine.setWakeAssistMode (settings.getIntValue ("wakeAssist", 0));

    if (settings.containsKey ("workers"))
        engine.presetWorkerThreads (settings.getIntValue ("workers", -1));

    auto deviceState = settings.getXmlValue ("audioDevice");
    engine.initialiseDevice (deviceState.get());
    engine.getDeviceManager().addChangeListener (this);

    session = std::make_unique<Session> (engine, pluginManager);
    session->onPluginAboutToBeDeleted = [] (juce::AudioPluginInstance* p) { PluginWindow::closeFor (p); };
    session->onPluginReplaced = [] (juce::AudioPluginInstance* o, juce::AudioPluginInstance* n) { PluginWindow::replace (o, n); };
    session->addChangeListener (this);

    edit = std::make_unique<EditController> (*session);
    edit->onRackSizeChanged = [this] (int) { layoutRacks(); };
    edit->onUserAction = [this] { grabKeyboardFocus(); };
    edit->addChangeListener (this);

    topBar = std::make_unique<TopBar> (*session);
    topBar->onNew      = [this] { newSession(); };
    topBar->onOpen     = [this] { openSession(); };
    topBar->onSave     = [this] { saveSession (false); };
    topBar->onPlugins  = [this]
    {
        if (pluginWindow == nullptr)
            pluginWindow = std::make_unique<PluginManagerWindow> (pluginManager);
        pluginWindow->setVisible (true);
        pluginWindow->toFront (true);
    };
    topBar->onSettings = [this]
    {
        if (settingsWindow == nullptr)
            settingsWindow = std::make_unique<SettingsWindow> (engine, pluginManager, *session, [this] (bool light) { setLightTheme (light); });
        settingsWindow->setVisible (true);
        settingsWindow->toFront (true);
    };
    topBar->onToggleLayout = [this] { setVerticalLayout (! verticalLayout); };
    addAndMakeVisible (*topBar);

    sceneList = std::make_unique<SceneListComponent> (*session);
    addAndMakeVisible (*sceneList);

    scenesToggle.onClick = [this] { setScenesCollapsed (! scenesCollapsed); };
    addAndMakeVisible (scenesToggle);

    rackViewport.setViewedComponent (&rackContainer, false);
    rackViewport.setScrollBarsShown (true, false);
    rackViewport.setScrollBarThickness (8);
    addAndMakeVisible (rackViewport);

    lookAndFeelChanged();
    addRackButton.onClick = [this] { session->addRack(); };
    rackContainer.addAndMakeVisible (addRackButton);

    // Reopen the last session: after a crash or reboot at a gig you're back in one click.
    const juce::File last (settings.getValue ("lastSession"));

    if (last.existsAsFile())
        session->loadFrom (last);
    else
        session->newSession();

    setVerticalLayout (settings.getBoolValue ("verticalLayout", false));
    setScenesCollapsed (settings.getBoolValue ("scenesCollapsed", false));
    setWantsKeyboardFocus (true);
    setSize (1440, 820);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    PluginWindow::closeAll();
    pluginWindow = nullptr;
    settingsWindow = nullptr;

    session->removeChangeListener (this);
    edit->removeChangeListener (this);
    engine.getDeviceManager().removeChangeListener (this);

    if (auto xml = engine.getDeviceManager().createStateXml())
        pluginManager.getSettings().setValue ("audioDevice", xml.get());

    rackComponents.clear();
    engine.clearRacks();
    edit = nullptr;
    session = nullptr;
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    topBar->setBounds (r.removeFromTop (TopBar::preferredHeight));

    if (! scenesCollapsed)
        sceneList->setBounds (r.removeFromLeft (240));

    scenesToggle.setBounds (r.removeFromLeft (16).reduced (0, 6));
    rackViewport.setBounds (r.reduced (6, 6));
    layoutRacks();
}

void MainComponent::setScenesCollapsed (bool collapsed)
{
    scenesCollapsed = collapsed;
    pluginManager.getSettings().setValue ("scenesCollapsed", collapsed);
    sceneList->setVisible (! collapsed);
    scenesToggle.collapsed = collapsed;
    scenesToggle.repaint();
    resized();
}

void MainComponent::lookAndFeelChanged()
{
    addRackButton.setColour (juce::TextButton::buttonColourId, theme::panel);
    addRackButton.setColour (juce::TextButton::textColourOffId, theme::textDim);
}

/** Switches every open window between the dark and light palette. */
void MainComponent::setLightTheme (bool light)
{
    theme::setLight (light);
    lnf.applyColours();

    auto& desktop = juce::Desktop::getInstance();

    for (int i = 0; i < desktop.getNumComponents(); ++i)
    {
        auto* c = desktop.getComponent (i);

        if (auto* w = dynamic_cast<juce::DocumentWindow*> (c))
            w->setBackgroundColour (theme::bg);

        c->sendLookAndFeelChange();
        c->repaint();
    }
}

void MainComponent::setVerticalLayout (bool v)
{
    verticalLayout = v;
    pluginManager.getSettings().setValue ("verticalLayout", v);
    topBar->setVerticalLayout (v);
    rackViewport.setScrollBarsShown (! v, v);
    addRackButton.setButtonText (v ? "+" : "+  ADD RACK");

    for (auto* rc : rackComponents)
        rc->setVertical (v);

    layoutRacks();
}

void MainComponent::layoutRacks()
{
    if (verticalLayout)
    {
        const int h = rackViewport.getMaximumVisibleHeight();
        int x = 0;

        for (auto* rc : rackComponents)
        {
            const auto w = rc->getPreferredLength();
            rc->setBounds (x, 0, w, h);
            x += w + 4;
        }

        addRackButton.setBounds (x + 4, 0, 48, h);
        rackContainer.setSize (x + 56, h);
        return;
    }

    const int w = rackViewport.getMaximumVisibleWidth();
    int y = 0;

    for (auto* rc : rackComponents)
    {
        const auto h = rc->getPreferredLength();
        rc->setBounds (0, y, w, h);
        y += h + 4;
    }

    addRackButton.setBounds (0, y + 4, w, 40);
    rackContainer.setSize (w, y + 52);
}

void MainComponent::syncRacks()
{
    bool same = rackComponents.size() == engine.getNumRacks();

    for (int i = 0; same && i < engine.getNumRacks(); ++i)
        same = &rackComponents[i]->getRack() == &engine.getRack (i);

    if (! same)
    {
        rackComponents.clear();

        for (int i = 0; i < engine.getNumRacks(); ++i)
        {
            auto* rc = rackComponents.add (new RackComponent (*session, *edit, engine.getRack (i), i));
            rc->setVertical (verticalLayout);
            rc->onResized = [this] { layoutRacks(); };
            rackContainer.addAndMakeVisible (rc);
        }

        layoutRacks();
    }
    else
    {
        for (auto* rc : rackComponents)
            rc->refresh();
    }
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == edit.get())
    {
        // selection only: repaint, no rebuild
        for (auto* rc : rackComponents)
            rc->repaint();
        return;
    }

    if (source == &engine.getDeviceManager())
    {
        if (auto xml = engine.getDeviceManager().createStateXml())
            pluginManager.getSettings().setValue ("audioDevice", xml.get());

        for (auto* rc : rackComponents)
            rc->refresh();

        topBar->repaint();
        return;
    }

    syncRacks();
    sceneList->refresh();
    topBar->refresh();
}

void MainComponent::timerCallback()
{
    // plugins can change their latency at any time: re-check groups twice a second
    if (++latencyCheckCounter % 15 == 0)
        engine.updateLatencyCompensation();

    topBar->updateMeters();

    for (auto* rc : rackComponents)
        if (rc->isShowing())
            rc->updateMeters();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto cmd = juce::ModifierKeys::commandModifier;

    if (key == juce::KeyPress::rightKey || key == juce::KeyPress::spaceKey || key == juce::KeyPress::pageDownKey)
    {
        session->stepScene (1);
        return true;
    }

    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::pageUpKey)
    {
        session->stepScene (-1);
        return true;
    }

    if (key == juce::KeyPress ('s', cmd | juce::ModifierKeys::shiftModifier, 0))
    {
        if (session->getCurrentScene() >= 0)
            session->storeScene (session->getCurrentScene());
        return true;
    }

    const auto alt = juce::ModifierKeys::altModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;

    // editing
    if (key == juce::KeyPress ('c', cmd, 0)) { edit->copy(); return true; }
    if (key == juce::KeyPress ('v', cmd, 0)) { edit->paste(); return true; }
    if (key == juce::KeyPress ('d', cmd, 0)) { edit->duplicate(); return true; }
    if (key == juce::KeyPress ('a', cmd, 0)) { edit->selectAllRacks(); return true; }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) { edit->deleteSelection(); return true; }
    if (key == juce::KeyPress::escapeKey) { edit->clearSelection(); return true; }

    // consecutive I/O on the selected racks
    if (key == juce::KeyPress ('i', cmd | alt, 0))   { edit->consecutiveInputs (false); return true; }
    if (key == juce::KeyPress ('i', cmd | shift, 0)) { edit->consecutiveInputs (true); return true; }
    if (key == juce::KeyPress ('o', cmd | alt, 0))   { edit->consecutiveOutputs (false); return true; }
    if (key == juce::KeyPress ('o', cmd | shift, 0)) { edit->consecutiveOutputs (true); return true; }

    // rack size (selected racks, or all)
    if (key == juce::KeyPress ('=', cmd, 0) || key == juce::KeyPress ('+', cmd, 0) || key == juce::KeyPress ('=', cmd | shift, 0))
    {
        edit->resizeTargets (1.15f);
        return true;
    }
    if (key == juce::KeyPress ('-', cmd, 0)) { edit->resizeTargets (1.0f / 1.15f); return true; }

    if (key == juce::KeyPress ('l', cmd, 0)) { setScenesCollapsed (! scenesCollapsed); return true; }
    if (key == juce::KeyPress ('s', cmd, 0)) { saveSession (false); return true; }
    if (key == juce::KeyPress ('o', cmd, 0)) { openSession(); return true; }
    if (key == juce::KeyPress ('n', cmd, 0)) { newSession(); return true; }
    if (key == juce::KeyPress ('r', cmd, 0)) { session->addRack(); return true; }

    // 1..9 recall scenes directly
    if (! key.getModifiers().isAnyModifierKeyDown())
    {
        const auto c = key.getTextCharacter();

        if (c >= '1' && c <= '9')
        {
            session->recallScene (c - '1');
            return true;
        }
    }

    return false;
}

//==============================================================================
void MainComponent::newSession()
{
    PluginWindow::closeAll();
    session->newSession();
    pluginManager.getSettings().removeValue ("lastSession");
}

void MainComponent::openSession()
{
    chooser = std::make_unique<juce::FileChooser> ("Open StageRack session",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   "*.stagerack");

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();

            if (f.existsAsFile())
            {
                PluginWindow::closeAll();
                session->loadFrom (f);
                pluginManager.getSettings().setValue ("lastSession", f.getFullPathName());
            }
        });
}

void MainComponent::saveSession (bool saveAs)
{
    if (! saveAs && session->getFile().existsAsFile())
    {
        session->saveTo (session->getFile());
        return;
    }

    chooser = std::make_unique<juce::FileChooser> ("Save StageRack session",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile ("Show.stagerack"),
                                                   "*.stagerack");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();

            if (f == juce::File())
                return;

            f = f.withFileExtension (".stagerack");

            if (session->saveTo (f))
                pluginManager.getSettings().setValue ("lastSession", f.getFullPathName());
        });
}

} // namespace stagerack
