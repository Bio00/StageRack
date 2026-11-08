#include "PluginManagerWindow.h"
#include "../Update/Updater.h"
#include "Theme.h"

namespace stagerack
{

//==============================================================================
class PluginManagerContent final : public juce::Component,
                                   private juce::Thread,
                                   private juce::Timer
{
public:
    explicit PluginManagerContent (PluginManager& pm)
        : juce::Thread ("StageRack scan"),
          plugins (pm),
          list (pm.getFormatManager(), pm.getKnownList(), pm.getDeadMansPedalFile(), &pm.getSettings(), true)
    {
        list.setNumberOfThreadsForScanning (scanThreads());
        list.setOptionsButtonText ("Options...");
        addAndMakeVisible (list);

        scanAll.setButtonText ("SCAN ALL FORMATS");
        scanAll.onClick = [this]
        {
            if (isThreadRunning())
                signalThreadShouldExit();
            else
                startThread();
        };
        addAndMakeVisible (scanAll);

        info.setFont (theme::font (12.0f));
        lookAndFeelChanged();
        addAndMakeVisible (info);
        setIdleText();
        setSize (780, 580);
    }

    void lookAndFeelChanged() override
    {
        scanAll.setColour (juce::TextButton::buttonColourId, theme::accent.withAlpha (0.3f));
        info.setColour (juce::Label::textColourId, theme::textDim);
    }

    ~PluginManagerContent() override
    {
        stopThread (10000);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::bg);

        if (isThreadRunning())
        {
            auto bar = progressArea.toFloat();
            g.setColour (theme::slot);
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (theme::accent);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, progress.load())), 3.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto top = r.removeFromTop (32);
        scanAll.setBounds (top.removeFromLeft (180));
        top.removeFromLeft (10);
        progressArea = top.removeFromBottom (6);
        info.setBounds (top);
        r.removeFromTop (6);
        list.setBounds (r);
    }

private:
    static int scanThreads() { return juce::jlimit (2, 8, juce::SystemStats::getNumPhysicalCpus() / 2); }

    void setIdleText()
    {
        info.setText (juce::String (plugins.getKnownList().getNumTypes()) + " plugins. Each plugin is scanned in its own process: "
                      "a plugin that crashes is blacklisted, StageRack keeps running.", juce::dontSendNotification);
    }

    // Background scan of every format, several child processes at a time.
    void run() override
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PluginManagerContent> (this)]
        {
            if (safe != nullptr) { safe->scanAll.setButtonText ("STOP"); safe->startTimerHz (10); }
        });

        auto& fm = plugins.getFormatManager();

        for (int f = 0; f < fm.getNumFormats() && ! threadShouldExit(); ++f)
        {
            auto* format = fm.getFormat (f);

            if (! format->canScanForPlugins())
                continue;

            juce::PluginDirectoryScanner scanner (plugins.getKnownList(), *format, format->getDefaultLocationsToSearch(),
                                                  true, plugins.getDeadMansPedalFile(), true);
            juce::ThreadPool pool (juce::ThreadPoolOptions{}.withNumberOfThreads (scanThreads()));
            std::atomic<bool> done { false };
            const auto formatName = format->getName();

            for (int t = 0; t < scanThreads(); ++t)
            {
                pool.addJob ([this, &scanner, &done, formatName]
                {
                    juce::String name;

                    while (! threadShouldExit() && ! done)
                    {
                        if (! scanner.scanNextFile (true, name))
                            done = true;

                        const juce::SpinLock::ScopedLockType sl (currentLock);
                        current = formatName + ": " + name;
                        progress = scanner.getProgress();
                    }
                });
            }

            while (pool.getNumJobs() > 0 && ! threadShouldExit())
                wait (100);

            pool.removeAllJobs (true, 60000);
        }

        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PluginManagerContent> (this)]
        {
            if (safe != nullptr)
            {
                safe->stopTimer();
                safe->scanAll.setButtonText ("SCAN ALL FORMATS");
                safe->setIdleText();
                safe->repaint();
            }
        });
    }

    void timerCallback() override
    {
        juce::String now;
        {
            // `current` is written by the scan threads: copy it under the same lock
            const juce::SpinLock::ScopedLockType sl (currentLock);
            now = current;
        }

        info.setText ("Scanning " + now + "   (" + juce::String (plugins.getKnownList().getNumTypes()) + " found)",
                      juce::dontSendNotification);
        repaint (progressArea);
    }

    PluginManager& plugins;
    juce::PluginListComponent list;
    juce::TextButton scanAll;
    juce::Label info;
    juce::Rectangle<int> progressArea;
    std::atomic<float> progress { 0.0f };
    juce::String current;
    juce::SpinLock currentLock;
};

PluginManagerWindow::PluginManagerWindow (PluginManager& pm)
    : juce::DocumentWindow ("Plugin Manager", theme::bg, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new PluginManagerContent (pm), true);
    setResizable (true, false);
    centreWithSize (getWidth(), getHeight());
}

//==============================================================================
class SettingsContent final : public juce::Component,
                              private juce::Timer
{
public:
    SettingsContent (AudioEngine& e, PluginManager& pm, Session& se, std::function<void (bool)> themeChanged,
                     std::function<void()> checkUpdatesFn)
        : engine (e), plugins (pm), session (se), onThemeChanged (std::move (themeChanged)),
          devices (e.getDeviceManager(), 0, 64, 0, 64, true, false, true, false)
    {
        addAndMakeVisible (devices);

        title.setText ("MULTICORE DSP", juce::dontSendNotification);
        title.setFont (theme::font (13.0f, true));
        addAndMakeVisible (title);

        lightTheme.setButtonText ("Light theme");
        lightTheme.setTooltip ("Light colours for daylight / outdoor stages. Dark is easier on the eyes in a dark venue.");
        lightTheme.setToggleState (theme::light, juce::dontSendNotification);
        lightTheme.onClick = [this]
        {
            plugins.getSettings().setValue ("lightTheme", lightTheme.getToggleState());
            if (onThemeChanged) onThemeChanged (lightTheme.getToggleState());
        };
        addAndMakeVisible (lightTheme);

        autoUpdate.setButtonText ("Check for updates at startup");
        autoUpdate.setTooltip ("Looks for a newer StageRack on GitHub when the app starts and asks before downloading anything.");
        autoUpdate.setToggleState (plugins.getSettings().getBoolValue (update::Controller::settingCheck, true), juce::dontSendNotification);
        autoUpdate.onClick = [this] { plugins.getSettings().setValue (update::Controller::settingCheck, autoUpdate.getToggleState()); };
        addAndMakeVisible (autoUpdate);

        onCheckUpdates = std::move (checkUpdatesFn);
        checkUpdates.setButtonText ("Check now");
        checkUpdates.onClick = [this] { if (onCheckUpdates) onCheckUpdates(); };
        addAndMakeVisible (checkUpdates);

        versionLabel.setText ("StageRack " + update::currentVersion(), juce::dontSendNotification);
        versionLabel.setFont (theme::font (11.0f));
        versionLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (versionLabel);

        const auto recommended = WorkerPool::getRecommendedWorkerCount();
        threads.addItem ("Auto (" + juce::String (recommended + 1) + (WorkerPool::hasPerformanceLevels() ? " threads on P-cores)" : " threads)"), 1);
        threads.addItem ("1 thread (no parallel processing)", 2);

        for (int n = 1; n < WorkerPool::maxThreads; ++n)
            threads.addItem (juce::String (n + 1) + " threads", n + 2);

        const auto setting = e.getWorkerThreadsSetting();
        threads.setSelectedId (setting < 0 ? 1 : setting + 2, juce::dontSendNotification);
        threads.onChange = [this]
        {
            const auto id = threads.getSelectedId();
            const auto workers = id == 1 ? -1 : id - 2;
            engine.setWorkerThreads (workers);
            plugins.getSettings().setValue ("workers", workers);
        };
        addAndMakeVisible (threads);
        threadsLabel.setText ("DSP threads", juce::dontSendNotification);
        addAndMakeVisible (threadsLabel);

        auto setupCC = [this] (juce::Slider& s, juce::Label& l, const juce::String& name, std::atomic<int>& target, const char* key)
        {
            s.setSliderStyle (juce::Slider::IncDecButtons);
            s.setRange (0, 127, 1);
            s.setValue (target.load(), juce::dontSendNotification);
            s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 50, 22);
            s.onValueChange = [this, &s, &target, key]
            {
                target = (int) s.getValue();
                plugins.getSettings().setValue (key, (int) s.getValue());
            };
            l.setText (name, juce::dontSendNotification);
            addAndMakeVisible (s);
            addAndMakeVisible (l);
        };

        setupCC (nextCC, nextLabel, "Next scene CC", engine.nextSceneCC, "nextSceneCC");
        setupCC (prevCC, prevLabel, "Previous scene CC", engine.prevSceneCC, "prevSceneCC");

        spread.setButtonText ("Spread heavy-plugin hops across callbacks (zero latency)");
        spread.setToggleState (engine.hopAlignEnabled.load(), juce::dontSendNotification);
        spread.setTooltip ("Frame-based plugins (e.g. De-Feedback) do their heavy work once per internal hop. "
                           "StageRack detects each plugin's hop and shifts it so instances never all work in the "
                           "same callback. Adds no latency and does not change the sound (measured: same level, "
                           "same timing). Switching it never interrupts the audio.");
        spread.onClick = [this]
        {
            engine.hopAlignEnabled = spread.getToggleState();
            plugins.getSettings().setValue ("hopAlign", spread.getToggleState());

            // ON: spread what is already loaded, live. OFF: nothing to undo (where a hop sits
            // does not change the sound); new plugins simply load without spreading.
            if (spread.getToggleState())
                session.respreadLive();
            else
                session.cancelRespread();
        };
        addAndMakeVisible (spread);

        respread.setButtonText ("Re-spread now");
        respread.setTooltip ("Re-measures and re-spreads all plugin hops while the audio keeps running: each plugin "
                             "hands over to a re-phased twin (same settings) with a crossfade. No dropout.");
        respread.onClick = [this] { if (engine.hopAlignEnabled) session.respreadLive(); };
        addAndMakeVisible (respread);
        respreadStatus.setFont (theme::font (11.0f));
        respreadStatus.setColour (juce::Label::textColourId, theme::textDim);
        addAndMakeVisible (respreadStatus);
        startTimerHz (4);

        spin.addItem ("Auto (around plugin hops)", 1);
        spin.addItem ("Always on", 2);
        spin.addItem ("Off", 3);
        spin.setSelectedId (engine.getWakeAssistMode() + 1, juce::dontSendNotification);
        spin.setTooltip ("Keeps P-cores awake just before heavy plugin hops, so plugins that wake their own helper "
                         "threads (e.g. De-Feedback) don't wait for a sleeping core. Fixes few-instance setups "
                         "at small buffers. Auto costs very little CPU.");
        spin.onChange = [this]
        {
            const int mode = spin.getSelectedId() - 1;
            engine.setWakeAssistMode (mode);
            plugins.getSettings().setValue ("wakeAssist", mode);
        };
        addAndMakeVisible (spin);
        spinLabel.setText ("Core wake assist", juce::dontSendNotification);
        addAndMakeVisible (spinLabel);

        midiInfo.setText ("MIDI Program Change N recalls scene N+1.", juce::dontSendNotification);
        addAndMakeVisible (midiInfo);

        lookAndFeelChanged();
        setSize (640, 910);
    }

    void lookAndFeelChanged() override
    {
        title.setColour (juce::Label::textColourId, theme::accent);
        midiInfo.setColour (juce::Label::textColourId, theme::textDim);
        versionLabel.setColour (juce::Label::textColourId, theme::textDim);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::bg); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto bottom = r.removeFromBottom (292);
        devices.setBounds (r);

        title.setBounds (bottom.removeFromTop (24));
        auto row = [&bottom] { auto x = bottom.removeFromTop (30); bottom.removeFromTop (4); return x; };

        auto r1 = row(); threadsLabel.setBounds (r1.removeFromLeft (160)); threads.setBounds (r1.removeFromLeft (300));
        auto rs = row(); respread.setBounds (rs.removeFromRight (120)); respreadStatus.setBounds (rs.removeFromRight (260)); spread.setBounds (rs);
        auto rk = row(); spinLabel.setBounds (rk.removeFromLeft (160)); spin.setBounds (rk.removeFromLeft (300));
        auto r2 = row(); nextLabel.setBounds (r2.removeFromLeft (160)); nextCC.setBounds (r2.removeFromLeft (140));
        auto r3 = row(); prevLabel.setBounds (r3.removeFromLeft (160)); prevCC.setBounds (r3.removeFromLeft (140));
        midiInfo.setBounds (row());
        lightTheme.setBounds (row().removeFromLeft (300));
        auto ru = row(); autoUpdate.setBounds (ru.removeFromLeft (260)); checkUpdates.setBounds (ru.removeFromLeft (100)); versionLabel.setBounds (ru);
    }

private:
    AudioEngine& engine;
    PluginManager& plugins;
    juce::AudioDeviceSelectorComponent devices;
    juce::Label title, threadsLabel, nextLabel, prevLabel, midiInfo;
    std::function<void (bool)> onThemeChanged;
    juce::ToggleButton spread, lightTheme, autoUpdate;
    juce::TextButton respread, checkUpdates;
    juce::Label versionLabel;
    std::function<void()> onCheckUpdates;
    juce::ComboBox spin;
    juce::Label spinLabel;

    Session& session;
    juce::Label respreadStatus;

    void timerCallback() override
    {
        respreadStatus.setText (session.getRespreadStatus(), juce::dontSendNotification);
        respread.setEnabled (engine.hopAlignEnabled && ! session.isRespreading());
    }
    juce::ComboBox threads;
    juce::Slider nextCC, prevCC;
};

SettingsWindow::SettingsWindow (AudioEngine& e, PluginManager& pm, Session& se, std::function<void (bool)> onThemeChanged,
                                std::function<void()> onCheckUpdates)
    : juce::DocumentWindow ("Settings", theme::bg, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new SettingsContent (e, pm, se, std::move (onThemeChanged), std::move (onCheckUpdates)), true);
    centreWithSize (getWidth(), getHeight());
}

} // namespace stagerack
