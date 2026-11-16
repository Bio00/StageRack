#include "MainComponent.h"
#include "TopBar.h"
#include "SceneListComponent.h"
#include "RackComponent.h"
#include "PluginWindow.h"
#include "PluginManagerWindow.h"
#include "AutomixerWindow.h"
#include "../Update/Updater.h"
#include "../Engine/CoreAudioProbe.h"

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

    // The first CoreAudio call blocks until coreaudiod answers. If coreaudiod is stuck (a HAL
    // driver spinning it), opening the device here would hang the app before any window shows:
    // ask CoreAudio from a background thread and open the device only once it has answered.
    audioReady = coreaudio::waitUntilReady (1500);

    if (audioReady)
        startAudioDevice();

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
    topBar->onPlugins  = [this] { openPluginManager(); };
    topBar->onSettings = [this] { openSettings(); };
    topBar->onToggleLayout = [this] { setVerticalLayout (! verticalLayout); };
    topBar->onAutomixView = [this] { openAutomixer(); };
    topBar->deviceStatus = [this] { return deviceGuard != nullptr ? deviceGuard->getStatus() : juce::String(); };
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
    addRackButton.setButtonText ("+  ADD RACK");
    addRackButton.setTooltip ("Add a rack (Cmd+R). Right-click: add several racks at once, on consecutive inputs");
    addRackButton.onClick = [this] { session->addRack(); };
    addRackButton.onRightClick = [this] { showAddRacksMenu(); };
    rackContainer.addAndMakeVisible (addRackButton);

    autosave = std::make_unique<Autosave> (*session);
    commonConstructorTail();
}

void MainComponent::openPluginManager()
{
    if (pluginWindow == nullptr)
        pluginWindow = std::make_unique<PluginManagerWindow> (pluginManager);
    pluginWindow->setVisible (true);
    pluginWindow->toFront (true);
}

void MainComponent::openAutomixer()
{
    if (automixerWindow == nullptr)
        automixerWindow = std::make_unique<AutomixerWindow> (*session);
    automixerWindow->setVisible (true);
    automixerWindow->toFront (true);
}

void MainComponent::toggleLightTheme()
{
    pluginManager.getSettings().setValue ("lightTheme", ! theme::light);
    setLightTheme (! theme::light);
}

void MainComponent::showAddRacksMenu()
{
    // inputs continue after the highest one in use
    int nextIn = 0;
    for (int i = 0; i < engine.getNumRacks(); ++i)
        if (auto& r = engine.getRack (i); r.inputRackId.load() < 0 && r.inputChannel.load() >= 0)
            nextIn = juce::jmax (nextIn, r.inputChannel.load() + (r.stereoInput.load() ? 2 : 1));

    juce::PopupMenu m;
    m.addSectionHeader ("Add racks on consecutive inputs, from IN " + juce::String (nextIn + 1));

    for (bool stereo : { false, true })
    {
        juce::PopupMenu sub;
        for (int n : { 2, 4, 6, 8, 12, 16, 24, 32 })
        {
            const auto last = nextIn + n * (stereo ? 2 : 1);
            sub.addItem (juce::String (n) + " racks  (IN " + juce::String (nextIn + 1) + "-" + juce::String (last) + ")",
                         [safe = juce::Component::SafePointer<MainComponent> (this), n, stereo]
                         { if (safe != nullptr) safe->session->addRacks (n, stereo); });
        }
        m.addSubMenu (stereo ? "Stereo racks" : "Mono racks", sub);
    }

    m.addSeparator();
    m.addItem ("Other number...", [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe == nullptr)
            return;

        auto* w = new juce::AlertWindow ("Add racks", "How many racks? They take consecutive inputs.", juce::MessageBoxIconType::NoIcon, safe.getComponent());
        w->addTextEditor ("n", "8", "Racks");
        w->addComboBox ("type", { "Mono inputs", "Stereo inputs" });
        w->addButton ("Add", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int result)
        {
            const auto n = juce::jlimit (0, 128, w->getTextEditorContents ("n").getIntValue());
            const bool stereo = w->getComboBoxComponent ("type")->getSelectedItemIndex() == 1;

            if (result == 1 && n > 0 && safe != nullptr)
                safe->session->addRacks (n, stereo);
        }), true);
    });

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addRackButton));
}

void MainComponent::openSettings()
{
    {
        if (! audioReady)
        {
            // the device list would block on coreaudiod too
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "CoreAudio is not responding",
                "coreaudiod has not answered yet, so the audio settings cannot be opened.\n"
                "If it lasts, a newly installed audio driver may be stuck: remove it or restart coreaudiod "
                "(sudo killall coreaudiod in Terminal).");
            return;
        }

        if (settingsWindow == nullptr)
            settingsWindow = std::make_unique<SettingsWindow> (engine, pluginManager, *session, [this] (bool light) { setLightTheme (light); },
                                                               [this] { updater->checkNow(); });
        settingsWindow->setVisible (true);
        settingsWindow->toFront (true);
    }
}

void MainComponent::commonConstructorTail()
{
    auto& settings = pluginManager.getSettings();

    if (audioReady)
    {
        loadStartupSession();
    }
    else
    {
        // plugins are prepared when the device starts: wait for CoreAudio before loading them
        session->newSession();
        waitingGeneration = session->getDocumentGeneration();
        addAndMakeVisible (coreAudioWaitLabel);
        coreAudioWaitLabel.setJustificationType (juce::Justification::centred);
        coreAudioWaitLabel.setFont (theme::font (15.0f, true));
        coreAudioWaitLabel.setColour (juce::Label::textColourId, theme::amber);
        coreAudioWaitLabel.setText ("Waiting for CoreAudio: coreaudiod is not responding.\n"
                                    "The session and the audio device open as soon as it answers.\n"
                                    "If it lasts, a newly installed audio driver may be stuck: "
                                    "remove it or restart coreaudiod (sudo killall coreaudiod).",
                                    juce::dontSendNotification);
    }

    setVerticalLayout (settings.getBoolValue ("verticalLayout", false));
    setScenesCollapsed (settings.getBoolValue ("scenesCollapsed", false));
    setWantsKeyboardFocus (true);
    setSize (1440, 820);
    startTimerHz (30);

    updater = std::make_unique<update::Controller> (settings, [] (std::function<void()> launchInstaller)
    {
        // the installer starts only once the app really quits (after "save changes?")
        update::setPendingInstall (std::move (launchInstaller));
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    });

    commands.registerAllCommandsForTarget (this);
    commands.setFirstCommandTarget (this);
    appMenu.setApplicationCommandManagerToWatch (&commands);

   #if JUCE_MAC
    juce::PopupMenu extra;
    extra.addItem ("Check for Updates...", [this] { updater->checkNow(); });
    extra.addSeparator();
    extra.addCommandItem (&commands, cmd::audioSettings, "Settings...");
    juce::MenuBarModel::setMacMainMenu (&appMenu, &extra);
   #endif

    // not for the command-line test modes (they never build the main window anyway)
    updater->checkAtStartup();
}

MainComponent::~MainComponent()
{
    prepareToQuit();
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void MainComponent::prepareToQuit()
{
    if (quitPrepared)
        return;

    quitPrepared = true;
    stopTimer();
    coreaudio::stopProbe();
    autosave->cleanExit();   // the user already answered "save changes?"
   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (nullptr);
   #endif
    updater = nullptr;
    PluginWindow::closeAll();
    pluginWindow = nullptr;
    settingsWindow = nullptr;
    automixerWindow = nullptr;

    session->removeChangeListener (this);
    edit->removeChangeListener (this);
    engine.getDeviceManager().removeChangeListener (this);

    // the device the user chose (not a fallback, never an empty state if CoreAudio never answered);
    // the guard goes before the device closes, or it would reopen it
    if (deviceGuard != nullptr)
    {
        if (auto* xml = deviceGuard->getPreferredState())
            pluginManager.getSettings().setValue ("audioDevice", xml);
        deviceGuard = nullptr;
    }

    pluginManager.saveAll();   // before any plugin is destroyed: a crash after this loses nothing

    rackComponents.clear();
    if (audioReady)
        engine.getDeviceManager().closeAudioDevice();   // no callback while the plugins go
    engine.clearRacks();
    edit = nullptr;
    session = nullptr;        // the plugins are destroyed here

    pluginManager.saveAll();
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
    coreAudioWaitLabel.setBounds (rackViewport.getBounds().removeFromTop (120));
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
        // the chosen device is saved by DeviceGuard (a fallback device must never overwrite it)
        for (auto* rc : rackComponents)
            rc->refresh();

        topBar->repaint();
        return;
    }

    syncRacks();
    sceneList->refresh();
    topBar->refresh();
    commands.commandStatusChanged();   // Undo/Redo and the ticks in the menu bar
}

void MainComponent::timerCallback()
{
    if (! audioReady && coreaudio::isReady())
    {
        audioReady = true;
        removeChildComponent (&coreAudioWaitLabel);
        startAudioDevice();

        // unless the user already started working while we waited
        if (session->getFile() == juce::File() && session->getDocumentGeneration() == waitingGeneration)
            loadStartupSession();
        else
            autosave->start();

        topBar->repaint();
    }

    // plugins can change their latency at any time: re-check groups twice a second
    if (++latencyCheckCounter % 15 == 0)
    {
        engine.updateLatencyCompensation();
        session->watchHops();
    }

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
    if (key == juce::KeyPress ('z', cmd, 0))         { session->undo(); return true; }
    if (key == juce::KeyPress ('z', cmd | shift, 0)) { session->redo(); return true; }
    if (key == juce::KeyPress ('m', cmd | shift, 0)) { engine.masterMuted = ! engine.masterMuted.load(); topBar->updateMeters(); return true; }
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
void MainComponent::startAudioDevice()
{
    deviceGuard = std::make_unique<DeviceGuard> (engine, pluginManager.getSettings().getXmlValue ("audioDevice"));
    deviceGuard->onPreferredChanged = [this] (const juce::XmlElement& xml)
    {
        pluginManager.getSettings().setValue ("audioDevice", &xml);
    };
    deviceGuard->askUseBuiltIn = [this] (std::function<void (bool)> answer)
    {
        auto options = juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle ("No audio interface chosen")
            .withMessage ("The only audio device available is the Mac's own (built-in speakers / microphone).\n\n"
                          "StageRack does not use it by itself: speakers and microphone in the same room can feed back.\n"
                          "Choose your interface in Settings, or use the built-in devices anyway.")
            .withButton ("Open Settings", 1)
            .withButton ("Use built-in anyway", 2)
            .withAssociatedComponent (this);

        juce::AlertWindow::showAsync (options, juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<MainComponent> (this), answer] (int result)
            {
                answer (result == 2);
                if (result == 1 && safe != nullptr)
                    safe->openSettings();
            }));
    };
    deviceGuard->start();
    engine.getDeviceManager().addChangeListener (this);
}

void MainComponent::loadStartupSession()
{
    // Reopen the last session: after a crash or reboot at a gig you're back in one click.
    // If the previous run crashed with unsaved changes, offer the autosave first (before
    // creating any plugin: loading the session twice would be slow).
    if (auto rec = Autosave::previousRunCrashed() ? Autosave::readRecovered() : Autosave::Recovered {}; rec.root.isValid())
    {
        session->newSession();
        offerRecovery (std::move (rec));
    }
    else
    {
        openLastSession();
        autosave->start();
    }
}

void MainComponent::openLastSession()
{
    const juce::File last (pluginManager.getSettings().getValue ("lastSession"));

    if (last.existsAsFile())
        session->loadFrom (last);
    else
        session->newSession();
}

void MainComponent::offerRecovery (Autosave::Recovered rec)
{
    const auto when = rec.savedAt.toString (rec.savedAt.toString (true, false) != juce::Time::getCurrentTime().toString (true, false),
                                            true, false, true);
    const auto name = rec.sourceFile.existsAsFile() ? "\"" + rec.sourceFile.getFileName() + "\"" : juce::String ("the untitled session");

    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "StageRack did not quit normally",
        "Changes to " + name + " were saved automatically at " + when + " but not saved to the session file.\n\n"
        "Restore them?",
        "Restore", "Open last saved", this,
        juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MainComponent> (this), rec] (int result)
        {
            if (safe == nullptr)
                return;

            if (result == 1)
            {
                safe->session->loadFromTree (rec.root, rec.sourceFile.existsAsFile() ? rec.sourceFile : juce::File(), true);
            }
            else
            {
                // keep it aside, in case it was the wrong button
                const auto f = Autosave::getAutosaveFile();
                f.moveFileTo (f.getSiblingFile ("Autosave (not restored).stagerack"));
                safe->openLastSession();
            }

            safe->autosave->start();
        }));
}

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

//==============================================================================
// Menu bar
juce::StringArray MainComponent::AppMenu::getMenuBarNames()
{
    return { "File", "Edit", "Rack", "Scene", "Audio", "View" };
}

juce::PopupMenu MainComponent::commandItems (std::initializer_list<juce::CommandID> ids)
{
    juce::PopupMenu m;

    for (auto id : ids)
    {
        if (id == 0)
        {
            m.addSeparator();
            continue;
        }

        juce::ApplicationCommandInfo info (id);
        getCommandInfo (id, info);

        juce::PopupMenu::Item item (info.shortName);
        item.itemID = (int) id;
        item.commandManager = &commands;
        item.isEnabled = (info.flags & juce::ApplicationCommandInfo::isDisabled) == 0;
        item.isTicked = (info.flags & juce::ApplicationCommandInfo::isTicked) != 0;
        item.customCallback = new KeyOriginCallback();   // remembers keyboard vs click
        m.addItem (item);
    }

    return m;
}

juce::PopupMenu MainComponent::AppMenu::getMenuForIndex (int index, const juce::String&)
{
    switch (index)
    {
        case 0:  return owner.commandItems ({ cmd::newSession, cmd::openSession, 0, cmd::saveSession, cmd::saveSessionAs });
        case 1:  return owner.commandItems ({ cmd::undo, cmd::redo, 0, cmd::copy, cmd::paste, cmd::duplicate, cmd::deleteSelection, 0,
                                              cmd::selectAll, cmd::deselect, 0,
                                              cmd::consecutiveInMono, cmd::consecutiveInStereo, cmd::consecutiveOutMono, cmd::consecutiveOutStereo });
        case 2:  return owner.commandItems ({ cmd::addRack, cmd::addRacks, 0, cmd::biggerRacks, cmd::smallerRacks });
        case 3:  return owner.commandItems ({ cmd::nextScene, cmd::prevScene, 0, cmd::storeScene, cmd::addScene });
        case 4:  return owner.commandItems ({ cmd::masterMute, cmd::automixOnOff, 0, cmd::audioSettings, cmd::pluginManager });
        case 5:  return owner.commandItems ({ cmd::toggleLayout, cmd::toggleScenes, cmd::automixerWindow, 0, cmd::lightTheme });
        default: return {};
    }
}

void MainComponent::getAllCommands (juce::Array<juce::CommandID>& ids)
{
    for (juce::CommandID id = cmd::newSession; id <= cmd::lightTheme; ++id)
        ids.add (id);
}

void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    const auto c = juce::ModifierKeys::commandModifier, s = juce::ModifierKeys::shiftModifier, a = juce::ModifierKeys::altModifier;
    const bool hasSelection = ! edit->targetRacks().isEmpty();
    auto set = [&] (const juce::String& name, const juce::String& category, int key = 0, int mods = 0)
    {
        info.setInfo (name, name, category, 0);
        if (key != 0)
            info.addDefaultKeypress (key, mods);
    };

    switch (id)
    {
        case cmd::newSession:          set ("New Session", "File", 'n', c); break;
        case cmd::openSession:         set ("Open...", "File", 'o', c); break;
        case cmd::saveSession:         set ("Save", "File", 's', c); break;
        case cmd::saveSessionAs:       set ("Save As...", "File", 's', c | a); break;
        case cmd::undo:                set (session->canUndo() ? "Undo" : "Can't Undo", "Edit", 'z', c); info.setActive (session->canUndo()); break;
        case cmd::redo:                set (session->canRedo() ? "Redo" : "Can't Redo", "Edit", 'z', c | s); info.setActive (session->canRedo()); break;
        case cmd::copy:                set ("Copy", "Edit", 'c', c); break;
        case cmd::paste:               set ("Paste", "Edit", 'v', c); break;
        case cmd::duplicate:           set ("Duplicate", "Edit", 'd', c); break;
        case cmd::deleteSelection:     set ("Delete", "Edit", juce::KeyPress::backspaceKey, 0); break;
        case cmd::selectAll:           set ("Select All Racks", "Edit", 'a', c); break;
        case cmd::deselect:            set ("Deselect", "Edit"); break;
        case cmd::consecutiveInMono:   set ("Consecutive Inputs (mono)", "Edit", 'i', c | a); info.setActive (hasSelection); break;
        case cmd::consecutiveInStereo: set ("Consecutive Inputs (stereo)", "Edit", 'i', c | s); info.setActive (hasSelection); break;
        case cmd::consecutiveOutMono:  set ("Consecutive Outputs (mono)", "Edit", 'o', c | a); info.setActive (hasSelection); break;
        case cmd::consecutiveOutStereo:set ("Consecutive Outputs (stereo)", "Edit", 'o', c | s); info.setActive (hasSelection); break;
        case cmd::addRack:             set ("Add Rack", "Rack", 'r', c); break;
        case cmd::addRacks:            set ("Add Several Racks...", "Rack"); break;
        case cmd::biggerRacks:         set ("Bigger Racks", "Rack", '=', c); break;
        case cmd::smallerRacks:        set ("Smaller Racks", "Rack", '-', c); break;
        case cmd::nextScene:           set ("Next Scene", "Scene"); break;
        case cmd::prevScene:           set ("Previous Scene", "Scene"); break;
        case cmd::storeScene:          set ("Store Scene", "Scene", 's', c | s); info.setActive (session->getCurrentScene() >= 0); break;
        case cmd::addScene:            set ("New Scene", "Scene"); break;
        case cmd::masterMute:          set ("Mute All Outputs", "Audio", 'm', c | s); info.setTicked (engine.masterMuted.load()); break;
        case cmd::automixOnOff:        set ("Automixer On", "Audio"); info.setTicked (engine.automixEnabled.load()); break;
        case cmd::audioSettings:       set ("Settings...", "Audio", ',', c); break;
        case cmd::pluginManager:       set ("Plugin Manager...", "Audio"); break;
        case cmd::toggleLayout:        set ("Vertical Racks", "View"); info.setTicked (verticalLayout); break;
        case cmd::toggleScenes:        set ("Scene List", "View", 'l', c); info.setTicked (! scenesCollapsed); break;
        case cmd::automixerWindow:     set ("Automixer Window", "View"); break;
        case cmd::lightTheme:          set ("Light Theme", "View"); info.setTicked (theme::light); break;
        default: break;
    }
}

bool MainComponent::perform (const InvocationInfo& info)
{
    // a shortcut typed in a plugin or settings window reaches the menu bar if that window did not
    // use it: never let it edit the racks (it would undo or delete something out of sight)
    if (cmd::isRackEdit (info.commandID) && KeyOriginCallback::lastWasKey)
    {
        KeyOriginCallback::lastWasKey = false;
        if (auto* peer = getPeer(); peer == nullptr || ! peer->isFocused())
            return true;
    }

    KeyOriginCallback::lastWasKey = false;

    switch (info.commandID)
    {
        case cmd::newSession:          newSession(); break;
        case cmd::openSession:         openSession(); break;
        case cmd::saveSession:         saveSession (false); break;
        case cmd::saveSessionAs:       saveSession (true); break;
        case cmd::undo:                session->undo(); break;
        case cmd::redo:                session->redo(); break;
        case cmd::copy:                edit->copy(); break;
        case cmd::paste:               edit->paste(); break;
        case cmd::duplicate:           edit->duplicate(); break;
        case cmd::deleteSelection:     edit->deleteSelection(); break;
        case cmd::selectAll:           edit->selectAllRacks(); break;
        case cmd::deselect:            edit->clearSelection(); break;
        case cmd::consecutiveInMono:   edit->consecutiveInputs (false); break;
        case cmd::consecutiveInStereo: edit->consecutiveInputs (true); break;
        case cmd::consecutiveOutMono:  edit->consecutiveOutputs (false); break;
        case cmd::consecutiveOutStereo:edit->consecutiveOutputs (true); break;
        case cmd::addRack:             session->addRack(); break;
        case cmd::addRacks:            showAddRacksMenu(); break;
        case cmd::biggerRacks:         edit->resizeTargets (1.15f); break;
        case cmd::smallerRacks:        edit->resizeTargets (1.0f / 1.15f); break;
        case cmd::nextScene:           session->stepScene (1); break;
        case cmd::prevScene:           session->stepScene (-1); break;
        case cmd::storeScene:          if (session->getCurrentScene() >= 0) session->storeScene (session->getCurrentScene()); break;
        case cmd::addScene:            session->addScene ("Scene " + juce::String (session->getNumScenes() + 1)); break;
        case cmd::masterMute:          engine.masterMuted = ! engine.masterMuted.load(); topBar->updateMeters(); break;
        case cmd::automixOnOff:        engine.automixEnabled = ! engine.automixEnabled.load(); session->markDirty(); break;
        case cmd::audioSettings:       openSettings(); break;
        case cmd::pluginManager:       openPluginManager(); break;
        case cmd::toggleLayout:        setVerticalLayout (! verticalLayout); break;
        case cmd::toggleScenes:        setScenesCollapsed (! scenesCollapsed); break;
        case cmd::automixerWindow:     openAutomixer(); break;
        case cmd::lightTheme:          toggleLightTheme(); break;
        default:                       return false;
    }

    commands.commandStatusChanged();
    return true;
}

} // namespace stagerack
