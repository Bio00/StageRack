#include "MainComponent.h"
#include "RackViewMenu.h"
#include "TopBar.h"
#include "SceneListComponent.h"
#include "RackComponent.h"
#include "PluginWindow.h"
#include "PluginManagerWindow.h"
#include "AutomixerWindow.h"
#include "TempoWindow.h"
#include "EditGroupsWindow.h"
#include "../Update/Updater.h"
#include "../Update/Changelog.h"
#include "ChangelogWindow.h"
#include "../Engine/CoreAudioProbe.h"

namespace stagerack
{

MainComponent::MainComponent()
{
    auto& settings = pluginManager.getSettings();
    theme::setLight (settings.getBoolValue ("lightTheme", false));
    theme::tintRacks = settings.getBoolValue ("tintRacks", true);
    theme::tapFlash = juce::jlimit (0, 4, settings.getIntValue ("tapFlash", 2));
    lnf.applyColours();
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

    engine.nextSceneCC = settings.getIntValue ("nextSceneCC", 80);
    engine.prevSceneCC = settings.getIntValue ("prevSceneCC", 81);
    engine.osc.setEnabled (settings.getBoolValue ("oscIn", false), settings.getIntValue ("oscPort", OscInput::defaultPort));
    Rack::asyncSafetyMs = (float) settings.getDoubleValue ("asyncSafetyMs", 3.0);
    engine.hopAlignEnabled = settings.getBoolValue ("hopAlign", true);
    engine.logWatch = std::getenv ("STAGERACK_WATCHLOG") != nullptr;   // diagnostics: print the hop watch decisions
    if (engine.logWatch) std::setvbuf (stdout, nullptr, _IOLBF, 0);
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
    topBar->onTempoView = [this] { openTempo(); };
    topBar->onGroupsView = [this] { openEditGroups(); };
    tapKeys = std::make_unique<TapKeys>();
    topBar->onToggleTheme = [this] { toggleLightTheme(); };
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
    rackViewport.addMouseListener (this, true);   // clicks on the empty space around the racks

    lookAndFeelChanged();
    addRackButton.setButtonText ("+  ADD RACK");
    addRackButton.setTooltip ("Add a rack (Cmd+R). Right-click: add several racks at once, on consecutive inputs");
    addRackButton.onClick = [this] { session->addRack(); };
    addRackButton.onRightClick = [this] { showAddRacksMenu(); };
    rackContainer.addAndMakeVisible (addRackButton);

    autosave = std::make_unique<Autosave> (*session);
    dropoutLog = std::make_unique<DropoutLog> (engine, PluginManager::getDataDirectory().getChildFile ("dropouts.log"));
    systemhealth::beginLiveActivity();   // no App Nap, no idle sleep, precise timers while we run
    systemhealth::memoryPressure();      // starts listening
    commonConstructorTail();
}

void MainComponent::openPluginManager()
{
    if (pluginWindow == nullptr)
        pluginWindow = std::make_unique<PluginManagerWindow> (pluginManager);
    pluginWindow->setVisible (true);
    pluginWindow->toFront (true);
}

void MainComponent::openEditGroups()
{
    if (editGroupsWindow == nullptr)
    {
        editGroupsWindow = std::make_unique<EditGroupsWindow> (*session, *edit);
        editGroupsWindow->getPanel().onChanged = [this]
        {
            for (auto* rc : rackComponents)
            {
                rc->resized();   // group badge in / out
                rc->repaint();
            }
            layoutRacks();
            topBar->updateMeters();
            commands.commandStatusChanged();
        };
    }

    editGroupsWindow->setVisible (true);
    editGroupsWindow->toFront (true);
}

void MainComponent::openTempo()
{
    if (tempoWindow == nullptr)
        tempoWindow = std::make_unique<TempoWindow> (*session);
    tempoWindow->setVisible (true);
    tempoWindow->toFront (true);
}

void MainComponent::openAutomixer()
{
    if (automixerWindow == nullptr)
        automixerWindow = std::make_unique<AutomixerWindow> (*session);
    automixerWindow->setVisible (true);
    automixerWindow->toFront (true);
}

void MainComponent::mouseDown (const juce::MouseEvent& e)
{
    // a click on empty space (not on a rack, a slot or a control) drops the selection
    auto* c = e.eventComponent;
    const bool empty = c == this || c == &rackViewport || c == &rackContainer
                    || c == rackContainer.getParentComponent();   // the viewport's content holder

    if (empty && ! e.mods.isPopupMenu())
        edit->clearSelection();
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
    m.addItem ("Add Bus (FX return, sub-group, master bus)", engine.firstFreeBus() > 0, false,
               [safe = juce::Component::SafePointer<MainComponent> (this)] { if (safe != nullptr) safe->session->addBus(); });
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
    extra.addItem ("What's New...", [] { showWhatsNew (false); });
    extra.addItem ("Check for Updates...", [this] { updater->checkNow(); });
    extra.addSeparator();
    extra.addCommandItem (&commands, cmd::audioSettings, "Settings...");
    juce::MenuBarModel::setMacMainMenu (&appMenu, &extra);
   #endif

    // not for the command-line test modes (they never build the main window anyway)
    updater->checkAtStartup();

    // first start of a new version: show what changed since the version the user last saw
    // (a first install shows nothing: there is no "before")
    const auto current = update::currentVersion();
    const auto seen = settings.getValue (update::settingSeenVersion);

    if (seen != current)
    {
        const bool upgraded = seen.isNotEmpty() ? update::compareVersions (seen, current) < 0
                                                : settings.containsKey ("lastSession") || settings.containsKey ("verticalLayout");
        settings.setValue (update::settingSeenVersion, current);

        if (upgraded)
            juce::Timer::callAfterDelay (1500, [safe = juce::Component::SafePointer<MainComponent> (this), seen]
            {
                if (safe != nullptr)
                    showWhatsNew (true, seen);
            });
    }
}

void MainComponent::showWhatsNew (bool sinceUpdate, const juce::String& seenBefore)
{
    const auto md = update::bundledChangelog();
    const auto current = update::currentVersion();

    if (sinceUpdate)
    {
        const auto notes = update::changesBetween (md, seenBefore, current);

        if (notes.isNotEmpty())
            ChangelogWindow::show ("StageRack " + current + ": what's new", notes);
        return;
    }

    // the whole history, from this version back (not the intro above the first version)
    juce::String all;
    for (const auto& e : update::parseChangelog (md))
        if (update::compareVersions (e.version, current) <= 0)
            all << "## " << e.version << (e.date.isNotEmpty() ? juce::String::fromUTF8 (" \xe2\x80\x94 ") + e.date : juce::String())
                << "\n\n" << e.body << "\n\n";

    ChangelogWindow::show ("What's New in StageRack",
                           all.isNotEmpty() ? all.trim() : juce::String ("The release notes are missing from this build."));
}

MainComponent::~MainComponent()
{
    rackViewport.removeMouseListener (this);
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
    ChangelogWindow::closeAll();
    PluginWindow::closeAll();
    pluginWindow = nullptr;
    settingsWindow = nullptr;
    automixerWindow = nullptr;
    tempoWindow = nullptr;
    editGroupsWindow = nullptr;

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
    auto depthOf = [this] (const Rack& r)
    {
        int d = 0;
        for (auto* f = r.folderId >= 0 ? engine.findRack (r.folderId) : nullptr; f != nullptr && d < 16;
             f = f->folderId >= 0 ? engine.findRack (f->folderId) : nullptr)
            ++d;
        return d;
    };

    const bool v = verticalLayout;
    const int across = v ? rackViewport.getMaximumVisibleHeight() : rackViewport.getMaximumVisibleWidth();
    int pos = 0;

    for (auto* rc : rackComponents)
    {
        // racks inside a closed folder are not shown (the folder still plays them)
        const bool shown = ! session->isHiddenInFolder (rc->getRack());
        rc->setVisible (shown);
        if (! shown) continue;

        if (pos > 0) pos += rc->getRack().folderId >= 0 ? 2 : 6;   // a folder's racks sit closer together
        const auto len = rc->getPreferredLength();
        const auto inset = depthOf (rc->getRack()) * folderStep;   // inside a folder: a step shorter, under its band

        if (v) rc->setBounds (pos, inset, len, across - inset);
        else   rc->setBounds (inset, pos, across - inset, len);
        pos += len;
    }

    // folder blocks: the folder rack + everything shown inside it
    rackContainer.vertical = v;
    rackContainer.folders.clear();

    for (int i = 0; i < rackComponents.size(); ++i)
    {
        auto* f = rackComponents[i];
        auto& fr = f->getRack();

        if (! fr.isFolder || ! f->isVisible())
            continue;

        auto block = f->getBounds();
        juce::Rectangle<int> members;

        for (int j = i + 1; j < rackComponents.size(); ++j)
        {
            auto* m = rackComponents[j];

            // members follow their folder (Session::normalizeFolders): stop at the first one outside
            bool inside = false;
            for (auto* p = m->getRack().folderId >= 0 ? engine.findRack (m->getRack().folderId) : nullptr; p != nullptr && ! inside;
                 p = p->folderId >= 0 ? engine.findRack (p->folderId) : nullptr)
                inside = p == &fr;

            if (! inside) break;
            if (m->isVisible()) members = members.isEmpty() ? m->getBounds() : members.getUnion (m->getBounds());
        }

        if (members.isEmpty())
            continue;   // closed (or empty): just the folder rack

        block = block.getUnion (members);
        const auto d = depthOf (fr) * folderStep;
        const auto band = v ? juce::Rectangle<int> (f->getRight() - 8, d, members.getRight() - f->getRight() + 8, folderStep - 3)
                            : juce::Rectangle<int> (d, f->getBottom() - 8, folderStep - 3, members.getBottom() - f->getBottom() + 8);
        rackContainer.folders.push_back ({ block, band, fr.colour, fr.name });
    }

    if (v)
    {
        addRackButton.setBounds (pos + 8, 0, 48, across);
        rackContainer.setSize (pos + 60, across);
    }
    else
    {
        addRackButton.setBounds (0, pos + 8, across, 40);
        rackContainer.setSize (across, pos + 56);
    }

    rackContainer.repaint();
}

void MainComponent::RackContainer::paint (juce::Graphics& g)
{
    for (auto& f : folders)
    {
        // a light tint behind the whole block, then the band that "holds" the racks
        g.setColour (f.colour.withAlpha (0.10f));
        g.fillRoundedRectangle (f.block.toFloat().expanded (2.0f), 7.0f);
        g.setColour (f.colour.withAlpha (0.45f));
        g.drawRoundedRectangle (f.block.toFloat().expanded (2.0f), 7.0f, 1.0f);

        g.setColour (f.colour.withAlpha (0.85f));
        g.fillRoundedRectangle (f.band.toFloat(), 4.0f);

        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.setFont (theme::font (10.0f, true));

        if (vertical)
        {
            g.drawText (f.name, f.band.withTrimmedLeft (14).reduced (4, 0), juce::Justification::centredLeft, true);
        }
        else
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, (float) f.band.getCentreX(), (float) f.band.getCentreY()));
            g.drawText (f.name, f.band.toFloat().withSizeKeepingCentre ((float) f.band.getHeight(), (float) f.band.getWidth()).reduced (14.0f, 0.0f),
                        juce::Justification::centredLeft, true);
        }
    }
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

        layoutRacks();   // a folder opened / closed
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

    // dropouts of the audio thread -> dropouts.log (with what the Mac was doing then)
    dropoutLog->poll ([this] (int id) { auto* r = engine.findRack (id); return r != nullptr ? r->name : juce::String (id); },
                      [this]
                      {
                          juce::String c = session->getFile().getFileName();
                          if (auto* dev = engine.getDeviceManager().getCurrentAudioDevice())
                              c << "  " << dev->getName() << " " << dev->getCurrentBufferSizeSamples() << " smp";
                          return c << "  " << engine.getNumRacks() << " racks";
                      });

    // Opening a session loads and aligns plugins (heavy work, a few glitches are normal and
    // nothing is playing yet): the counter shown starts once that is over. The log keeps all.
    {
        const bool settling = ! audioReady || session->getPendingLoads() > 0 || session->isRespreading();
        if (settling && ! sessionSettling)
            dropoutLog->note ("loading / aligning plugins");
        if (sessionSettling && ! settling)
        {
            engine.dropouts.clearCounters();
            dropoutLog->note ("session ready: counter restarted");
        }
        sessionSettling = settling;
    }

    engine.tempo.poll();   // audio taps / beat tracking (timestamps are exact: the timer's lag doesn't matter)
    engine.tempoOut.update();   // MIDI / OSC out of the tempos: settings, devices
    topBar->updateMeters();
    edit->syncGroupParameters();   // a knob turned in a plugin window: same knob in its edit group

    for (auto* rc : rackComponents)
        if (rc->isShowing())
            rc->updateMeters();
}

bool MainComponent::keyStateChanged (bool)
{
    tapKeys->keyStateChanged();
    return false;
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    // tap tempo keys first (plain keys only; repeats of a held key are not taps)
    if (tapKeys->keyPressed (engine.tempo, key))
        return true;

    if (key == juce::KeyPress ('t', juce::ModifierKeys::commandModifier, 0)) { openTempo(); return true; }

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
    if (key == juce::KeyPress ('g', cmd | shift, 0)) { commands.invokeDirectly (cmd::editGroupsOn, false); return true; }
    if (key == juce::KeyPress ('b', cmd | shift, 0)) { commands.invokeDirectly (cmd::addBus, false); return true; }
    if (key == juce::KeyPress ('g', cmd, 0))         { commands.invokeDirectly (cmd::groupFolder, false); return true; }
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
        case 2:  return owner.commandItems ({ cmd::addRack, cmd::addRacks, cmd::addBus, cmd::groupFolder, 0, cmd::routingOnOff, 0, cmd::biggerRacks, cmd::smallerRacks, 0, cmd::editGroupsWindow, cmd::editGroupsOn });
        case 3:  return owner.commandItems ({ cmd::nextScene, cmd::prevScene, 0, cmd::storeScene, cmd::addScene });
        case 4:  return owner.commandItems ({ cmd::masterMute, cmd::automixOnOff, cmd::tapMaster, cmd::addTempo, 0, cmd::audioSettings, cmd::pluginManager });
        case 5:
        {
            auto m = owner.commandItems ({ cmd::toggleLayout, cmd::toggleScenes, cmd::automixerWindow, cmd::tempoWindow, 0, cmd::lightTheme, cmd::tintRacks });
            m.addSeparator();
            m.addSubMenu ("Rack View", rackViewMenu (*owner.session));
            return m;
        }
        default: return {};
    }
}

void MainComponent::getAllCommands (juce::Array<juce::CommandID>& ids)
{
    for (juce::CommandID id = cmd::newSession; id <= cmd::lastCommand; ++id)
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
        case cmd::tempoWindow:         set ("Tempo Window", "View", 't', c); break;
        case cmd::tapMaster:           set ("Tap Master Tempo", "Audio"); break;
        case cmd::addTempo:            set ("Add Tap Tempo", "Audio"); info.setActive (engine.tempo.getNumTempos() < TempoEngine::maxTempos); break;
        case cmd::editGroupsWindow:    set ("Edit Groups...", "Rack"); break;
        case cmd::routingOnOff:        set ("Buses and Sends", "Rack"); info.setTicked (engine.isRoutingEnabled()); break;
        case cmd::addBus:              set ("Add Bus", "Rack", 'b', c | s); info.setActive (engine.firstFreeBus() > 0); break;
        case cmd::groupFolder:         set ("Group Into Folder", "Rack", 'g', c); info.setActive (engine.firstFreeBus() > 0 && engine.getNumRacks() > 0); break;
        case cmd::lightTheme:          set ("Light Theme", "View"); info.setTicked (theme::light); break;
        case cmd::tintRacks:           set ("Tinted Racks", "View"); info.setTicked (theme::tintRacks); break;
        case cmd::editGroupsOn:
        {
            bool anyUsed = false, anyOn = false;
            for (int g = 1; g <= Rack::maxEditGroups; ++g)
                if (session->numRacksInEditGroup (g) > 0) { anyUsed = true; anyOn = anyOn || session->isEditGroupActive (g); }
            set ("Edit Groups Active", "Rack", 'g', c | s);
            info.setActive (anyUsed);
            info.setTicked (anyOn);
            break;
        }
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
        case cmd::tempoWindow:         openTempo(); break;
        case cmd::tapMaster:           engine.tempo.tapNow (0, TempoEngine::srcButton); break;
        case cmd::addTempo:            session->addTempo(); break;
        case cmd::editGroupsWindow:    openEditGroups(); break;
        case cmd::routingOnOff:        session->setRoutingEnabled (! engine.isRoutingEnabled()); break;
        case cmd::addBus:              session->addBus(); break;
        case cmd::groupFolder:
        {
            auto ids = edit->targetRacks();
            if (! ids.isEmpty()) session->groupIntoFolder (ids);
            break;
        }
        case cmd::lightTheme:          toggleLightTheme(); break;
        case cmd::tintRacks:
            theme::tintRacks = ! theme::tintRacks;
            pluginManager.getSettings().setValue ("tintRacks", theme::tintRacks);
            repaint();
            for (auto* c : getChildren()) c->repaint();
            break;
        case cmd::editGroupsOn:
        {
            // suspend all groups (one click before a solo edit) / resume them
            bool anyOn = false;
            for (int g = 1; g <= Rack::maxEditGroups; ++g)
                anyOn = anyOn || (session->numRacksInEditGroup (g) > 0 && session->isEditGroupActive (g));
            for (int g = 1; g <= Rack::maxEditGroups; ++g)
                if (session->numRacksInEditGroup (g) > 0 && session->isEditGroupActive (g) == anyOn)
                    session->setEditGroupActive (g, ! anyOn);
            break;
        }
        default:                       return false;
    }

    commands.commandStatusChanged();
    return true;
}

} // namespace stagerack
