#include "PluginManagerWindow.h"
#include "../Update/Updater.h"
#include "Theme.h"
#include "MainComponent.h"

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
/** JUCE's device selector, with the input rows (device, active channels) above the output ones:
    signal flow order, input first. JUCE lays them out output-first inside a private panel, so the
    rows are swapped after every layout pass (idempotent). */
class InputFirstDeviceSelector final : public juce::AudioDeviceSelectorComponent
{
public:
    InputFirstDeviceSelector (juce::AudioDeviceManager& dm, int minIn, int maxIn, int minOut, int maxOut)
        : juce::AudioDeviceSelectorComponent (dm, minIn, maxIn, minOut, maxOut, true, false, true, false), manager (dm)
    {
        manager.addChangeListener (&deviceListener);
    }

    ~InputFirstDeviceSelector() override { manager.removeChangeListener (&deviceListener); }

    void resized() override
    {
        juce::AudioDeviceSelectorComponent::resized();
        swapRows();
    }

    void childBoundsChanged (juce::Component* c) override
    {
        juce::AudioDeviceSelectorComponent::childBoundsChanged (c);
        swapRows();
    }

private:
    // the panel re-lays itself out on device changes: fix it afterwards
    struct DeviceListener final : juce::ChangeListener
    {
        InputFirstDeviceSelector& owner;
        explicit DeviceListener (InputFirstDeviceSelector& o) : owner (o) {}
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            juce::Component::SafePointer<InputFirstDeviceSelector> safe (&owner);
            juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->swapRows(); });
        }
    } deviceListener { *this };

    static juce::Component* attachedTo (juce::Component& panel, const juce::String& labelText)
    {
        for (auto* c : panel.getChildren())
            if (auto* l = dynamic_cast<juce::Label*> (c); l != nullptr && l->getText() == labelText)
                return l->getAttachedComponent();
        return nullptr;
    }

    /** Puts the row of `in` where the row of `out` is (and vice versa), if `out` is above. */
    static void swapPair (juce::Component& panel, juce::Component* out, juce::Component* in)
    {
        if (out == nullptr || in == nullptr || out->getY() >= in->getY())
            return;

        const int top = out->getY(), gap = in->getY() - out->getBottom();
        const int outY = out->getY(), inY = in->getY();
        const int newOutY = top + in->getHeight() + gap;
        juce::Array<juce::Component*> outRow, inRow;

        for (auto* c : panel.getChildren())
        {
            if (dynamic_cast<juce::Label*> (c) != nullptr)
                continue;   // attached labels follow their component
            if (c->getY() == outY) outRow.add (c);
            else if (c->getY() == inY) inRow.add (c);
        }

        for (auto* c : inRow)  c->setTopLeftPosition (c->getX(), top);
        for (auto* c : outRow) c->setTopLeftPosition (c->getX(), newOutY);
    }

    void swapRows()
    {
        for (auto* panel : getChildren())
        {
            swapPair (*panel, attachedTo (*panel, "Output:"), attachedTo (*panel, "Input:"));
            swapPair (*panel, attachedTo (*panel, "Active output channels:"), attachedTo (*panel, "Active input channels:"));
        }
    }

    juce::AudioDeviceManager& manager;
};

//==============================================================================
class SettingsContent final : public juce::Component,
                              private juce::Timer
{
public:
    enum PageId { pageAudio, pagePerformance, pageMidi, pageAppearance, pageUpdates, numPages };

    SettingsContent (AudioEngine& e, PluginManager& pm, Session& se, std::function<void (bool)> themeChanged,
                     std::function<void()> checkUpdatesFn)
        : engine (e), plugins (pm), session (se), onThemeChanged (std::move (themeChanged)),
          onCheckUpdates (std::move (checkUpdatesFn)),
          devices (e.getDeviceManager(), 0, 64, 0, 64)
    {
        const char* names[numPages] = { "Audio device", "Performance", "MIDI", "Appearance", "Updates" };
        for (int i = 0; i < numPages; ++i)
        {
            pages[i].owner = this;
            sidebar.names.add (names[i]);
        }

        sidebar.onSelect = [this] (int i) { showPage (i); };
        addAndMakeVisible (sidebar);
        view.setScrollBarsShown (true, false);
        view.setScrollBarThickness (8);
        addAndMakeVisible (view);

        // ---- Audio device ----------------------------------------------------------------------
        pages[pageAudio].add (devices);
        pages[pageAudio].layout = [this] (int w)
        {
            devices.setBounds (0, 6, w, 2000);   // the selector shrinks itself to its content
            return devices.getBottom() + 12;
        };

        // ---- Performance -------------------------------------------------------------------------
        auto& perf = pages[pagePerformance];
        heading (perf, dspTitle, "MULTICORE DSP");

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
        label (perf, threadsLabel, "DSP threads");
        perf.add (threads);

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
        perf.add (spread);
        note (perf, spreadNote, "Plugins that compute in bursts (De-Feedback, EverClean...) are moved so they never "
                                "all work in the same callback. No latency, no change to the sound.");

        respread.setButtonText ("Re-spread now");
        respread.setTooltip ("Re-measures and re-spreads all plugin hops while the audio keeps running: each plugin "
                             "hands over to a re-phased twin (same settings) with a crossfade. No dropout.");
        respread.onClick = [this] { if (engine.hopAlignEnabled) session.respreadLive(); };
        perf.add (respread);
        respreadStatus.setFont (theme::font (11.0f));
        perf.add (respreadStatus);

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
        label (perf, spinLabel, "Core wake assist");
        perf.add (spin);

        heading (perf, isolateTitle, "PLUGIN ISOLATION  (EXPERIMENTAL)");
        isolate.setButtonText ("Run each plugin in its own process");
        isolate.setToggleState (PluginManager::isolatePlugins.load(), juce::dontSendNotification);
        isolate.onClick = [this] { isolateClicked(); };
        perf.add (isolate);
        note (perf, isolateNote, "If an isolated plugin crashes, only its slot passes the sound dry and the plugin comes "
                                 "back by itself with its settings. No added latency. Applies to plugins loaded from now "
                                 "on: reopen the session to apply it to all of them.");
        note (perf, isolateWarning, "Costs a lot of CPU: about 5% of a core more for every plugin, and the slowest "
                                    "callback roughly doubles (measured: 6 plugins, total CPU 46% \xe2\x86\x92 76%). "
                                    "Leave it off unless a plugin keeps crashing.");

        perf.layout = [this] (int w)
        {
            Rows r (w);
            dspTitle.setBounds (r.row (24));
            auto t = r.row (28); threadsLabel.setBounds (t.removeFromLeft (150)); threads.setBounds (t.removeFromLeft (300));
            r.gap (8);
            spread.setBounds (r.row (26));
            spreadNote.setBounds (r.text (spreadNote, 26));
            auto rs = r.row (28); rs.removeFromLeft (26); respread.setBounds (rs.removeFromLeft (130)); rs.removeFromLeft (10); respreadStatus.setBounds (rs);
            r.gap (8);
            auto k = r.row (28); spinLabel.setBounds (k.removeFromLeft (150)); spin.setBounds (k.removeFromLeft (300));
            r.gap (22);
            isolateTitle.setBounds (r.row (24));
            isolate.setBounds (r.row (26));
            isolateNote.setBounds (r.text (isolateNote, 26));
            r.gap (4);
            isolateWarning.setBounds (r.text (isolateWarning, 26));
            return r.y + 12;
        };

        // ---- MIDI ----------------------------------------------------------------------------------
        auto& midi = pages[pageMidi];
        heading (midi, sceneTitle, "SCENES FROM MIDI");
        auto setupCC = [this, &midi] (juce::Slider& s, juce::Label& l, const juce::String& name, std::atomic<int>& target, const char* key)
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
            label (midi, l, name);
            midi.add (s);
        };
        setupCC (nextCC, nextLabel, "Next scene CC", engine.nextSceneCC, "nextSceneCC");
        setupCC (prevCC, prevLabel, "Previous scene CC", engine.prevSceneCC, "prevSceneCC");
        note (midi, midiInfo, "MIDI Program Change N recalls scene N+1. Controllers, faders and buttons are mapped in "
                              "the MIDI window (MIDI in the top bar, \xe2\x8c\x98\xe2\x8c\xa5M) or with MIDI MAP (\xe2\x8c\x98M).");
        midi.layout = [this] (int w)
        {
            Rows r (w);
            sceneTitle.setBounds (r.row (24));
            auto a = r.row (28); nextLabel.setBounds (a.removeFromLeft (150)); nextCC.setBounds (a.removeFromLeft (140));
            auto b = r.row (28); prevLabel.setBounds (b.removeFromLeft (150)); prevCC.setBounds (b.removeFromLeft (140));
            r.gap (6);
            midiInfo.setBounds (r.text (midiInfo, 0));
            return r.y + 12;
        };

        // ---- Appearance ----------------------------------------------------------------------------
        auto& look = pages[pageAppearance];
        heading (look, lookTitle, "APPEARANCE");
        lightTheme.setButtonText ("Light theme");
        lightTheme.setToggleState (theme::light, juce::dontSendNotification);
        lightTheme.onClick = [this]
        {
            plugins.getSettings().setValue ("lightTheme", lightTheme.getToggleState());
            if (onThemeChanged) onThemeChanged (lightTheme.getToggleState());
        };
        look.add (lightTheme);
        note (look, lightNote, "Light colours for daylight and outdoor stages. Dark is easier on the eyes in a dark venue.");

        tapFlash.addItemList ({ "Off", "Subtle", "Medium", "Strong", "Full" }, 1);
        tapFlash.setSelectedItemIndex (theme::tapFlash, juce::dontSendNotification);
        tapFlash.onChange = [this]
        {
            theme::tapFlash = tapFlash.getSelectedItemIndex();
            plugins.getSettings().setValue ("tapFlash", theme::tapFlash);
        };
        label (look, tapFlashLabel, "TAP beat flash");
        look.add (tapFlash);
        note (look, tapFlashNote, "How much the TAP buttons in the top bar light up on each beat.");
        look.layout = [this] (int w)
        {
            Rows r (w);
            lookTitle.setBounds (r.row (24));
            lightTheme.setBounds (r.row (26));
            lightNote.setBounds (r.text (lightNote, 26));
            r.gap (10);
            auto t = r.row (28); tapFlashLabel.setBounds (t.removeFromLeft (150)); tapFlash.setBounds (t.removeFromLeft (160));
            tapFlashNote.setBounds (r.text (tapFlashNote, 150));
            return r.y + 12;
        };

        // ---- Updates -------------------------------------------------------------------------------
        auto& upd = pages[pageUpdates];
        heading (upd, updatesTitle, "UPDATES");
        autoUpdate.setButtonText ("Check for updates at startup");
        autoUpdate.setToggleState (plugins.getSettings().getBoolValue (update::Controller::settingCheck, true), juce::dontSendNotification);
        autoUpdate.onClick = [this] { plugins.getSettings().setValue (update::Controller::settingCheck, autoUpdate.getToggleState()); };
        upd.add (autoUpdate);
        note (upd, updateNote, "Looks for a newer StageRack on GitHub when the app starts and asks before downloading anything.");
        checkUpdates.setButtonText ("Check now");
        checkUpdates.onClick = [this] { if (onCheckUpdates) onCheckUpdates(); };
        upd.add (checkUpdates);
        changelog.setButtonText ("Changelog");
        changelog.setTooltip ("Release notes of every StageRack version up to this one");
        changelog.onClick = [] { MainComponent::showWhatsNew (false); };
        upd.add (changelog);
        versionLabel.setText ("StageRack " + update::currentVersion(), juce::dontSendNotification);
        versionLabel.setFont (theme::font (12.0f));
        upd.add (versionLabel);
        upd.layout = [this] (int w)
        {
            Rows r (w);
            updatesTitle.setBounds (r.row (24));
            versionLabel.setBounds (r.row (22));
            r.gap (6);
            autoUpdate.setBounds (r.row (26));
            updateNote.setBounds (r.text (updateNote, 26));
            r.gap (6);
            auto b = r.row (28); checkUpdates.setBounds (b.removeFromLeft (120)); b.removeFromLeft (8); changelog.setBounds (b.removeFromLeft (120));
            return r.y + 12;
        };

        lookAndFeelChanged();
        startTimerHz (4);
        setSize (760, 560);
        showPage (juce::jlimit (0, numPages - 1, plugins.getSettings().getIntValue ("settingsPage", 0)));
    }

    void lookAndFeelChanged() override
    {
        for (auto* l : { &dspTitle, &isolateTitle, &sceneTitle, &lookTitle, &updatesTitle })
            l->setColour (juce::Label::textColourId, theme::accent);
        for (auto* l : { &spreadNote, &isolateNote, &midiInfo, &lightNote, &tapFlashNote, &updateNote, &respreadStatus, &versionLabel })
            l->setColour (juce::Label::textColourId, theme::textDim);
        isolateWarning.setColour (juce::Label::textColourId, theme::amber);
        repaint();
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::bg); }

    void resized() override
    {
        auto r = getLocalBounds();
        sidebar.setBounds (r.removeFromLeft (170));
        view.setBounds (r.reduced (16, 8));
        relayoutPage();
    }

    void relayoutPage()
    {
        auto& p = pages[current];
        const int w = juce::jmax (200, view.getMaximumVisibleWidth() - 4);
        p.setSize (w, 10);
        const int h = p.layout ? p.layout (w) : 0;
        p.setSize (w, juce::jmax (h, view.getHeight()));
    }

private:
    /** One page of settings: lays its controls out for a width and says how tall it is. */
    struct Page final : juce::Component
    {
        SettingsContent* owner = nullptr;
        std::function<int (int width)> layout;
        bool layingOut = false;

        void add (juce::Component& c) { addAndMakeVisible (c); }
        void paint (juce::Graphics& g) override { g.fillAll (theme::bg); }
        void childBoundsChanged (juce::Component*) override
        {
            // the device selector resizes itself when the device changes
            if (owner != nullptr && ! layingOut && isShowing())
            {
                juce::Component::SafePointer<SettingsContent> safe (owner);
                juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->relayoutPage(); });
            }
        }
    };

    struct Rows
    {
        explicit Rows (int width) : w (width) {}
        int w, y = 6;
        juce::Rectangle<int> row (int h) { auto r = juce::Rectangle<int> (0, y, w, h); y += h + 4; return r; }
        void gap (int h) { y += h; }
        /** A wrapped text block, indented by `indent`, as tall as its lines. */
        juce::Rectangle<int> text (juce::Label& l, int indent)
        {
            const int tw = juce::jmax (60, w - indent);
            juce::AttributedString s;
            s.append (l.getText(), l.getFont());
            juce::TextLayout tl;
            tl.createLayout (s, (float) tw - 6.0f);
            const int h = (int) std::ceil (tl.getHeight()) + 6;
            auto r = juce::Rectangle<int> (indent, y, tw, h);
            y += h + 4;
            return r;
        }
    };

    struct Sidebar final : juce::Component
    {
        juce::StringArray names;
        int selected = 0;
        std::function<void (int)> onSelect;

        int itemAt (juce::Point<int> p) const { return p.y < 12 ? -1 : (p.y - 12) / 34; }
        void paint (juce::Graphics& g) override
        {
            g.fillAll (theme::panel);
            g.setColour (theme::outline);
            g.fillRect (getWidth() - 1, 0, 1, getHeight());
            for (int i = 0; i < names.size(); ++i)
            {
                auto r = juce::Rectangle<int> (8, 12 + i * 34, getWidth() - 17, 30);
                if (i == selected)
                {
                    g.setColour (theme::panelLight);
                    g.fillRoundedRectangle (r.toFloat(), 5.0f);
                    g.setColour (theme::accent);
                    g.fillRoundedRectangle (r.removeFromLeft (3).reduced (0, 6).toFloat(), 1.5f);
                }
                else if (i == hover)
                {
                    g.setColour (theme::panelLight.withAlpha (0.5f));
                    g.fillRoundedRectangle (r.toFloat(), 5.0f);
                }
                g.setColour (i == selected ? theme::text : theme::textDim);
                g.setFont (theme::font (13.5f, i == selected));
                g.drawText (names[i], r.withTrimmedLeft (14), juce::Justification::centredLeft);
            }
        }
        void mouseDown (const juce::MouseEvent& e) override
        {
            const int i = itemAt (e.getPosition());
            if (i >= 0 && i < names.size() && onSelect) onSelect (i);
        }
        void mouseMove (const juce::MouseEvent& e) override { setHover (itemAt (e.getPosition())); }
        void mouseExit (const juce::MouseEvent&) override { setHover (-1); }
        void setHover (int i) { if (i != hover) { hover = i; repaint(); } }
        int hover = -1;
    };

public:
    void showPage (int i)
    {
        current = i;
        sidebar.selected = i;
        sidebar.repaint();
        view.setViewedComponent (&pages[i], false);
        view.setViewPosition (0, 0);
        relayoutPage();
        plugins.getSettings().setValue ("settingsPage", i);
    }

private:
    void heading (Page& p, juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (theme::font (13.0f, true));
        p.add (l);
    }

    void label (Page& p, juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        p.add (l);
    }

    void note (Page& p, juce::Label& l, const char* utf8)
    {
        l.setText (juce::String::fromUTF8 (utf8), juce::dontSendNotification);
        l.setFont (theme::font (12.0f));
        l.setJustificationType (juce::Justification::topLeft);
        l.setMinimumHorizontalScale (1.0f);
        p.add (l);
    }

    void isolateClicked()
    {
        auto apply = [this] (bool on)
        {
            isolate.setToggleState (on, juce::dontSendNotification);
            PluginManager::isolatePlugins = on;
            plugins.getSettings().setValue ("isolatePlugins", on);
        };

        if (! isolate.getToggleState())
        {
            apply (false);
            return;
        }

        isolate.setToggleState (false, juce::dontSendNotification);   // only once confirmed
        juce::Component::SafePointer<SettingsContent> safe (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Isolate plugins? (experimental)")
                                          .withMessage ("Every plugin loaded from now on runs in its own process.\n\n"
                                                        "It costs a lot of CPU: about 5% of a core more per plugin, and the slowest "
                                                        "callback roughly doubles. At small buffers it can cause dropouts that "
                                                        "you would not have otherwise.\n\n"
                                                        "Use it only if a plugin keeps crashing.")
                                          .withButton ("Isolate plugins", 1)
                                          .withButton ("Cancel", 0)
                                          .withAssociatedComponent (this),
                                      [safe, apply] (int result)
                                      {
                                          if (safe != nullptr && result == 1)
                                              apply (true);
                                      });
    }

    void timerCallback() override
    {
        respreadStatus.setText (session.getRespreadStatus(), juce::dontSendNotification);
        respread.setEnabled (engine.hopAlignEnabled && ! session.isRespreading());
    }

    AudioEngine& engine;
    PluginManager& plugins;
    Session& session;
    std::function<void (bool)> onThemeChanged;
    std::function<void()> onCheckUpdates;

    Sidebar sidebar;
    juce::Viewport view;
    Page pages[numPages];
    int current = 0;

    InputFirstDeviceSelector devices;
    juce::Label dspTitle, threadsLabel, spreadNote, respreadStatus, spinLabel, isolateTitle, isolateNote, isolateWarning;
    juce::Label sceneTitle, nextLabel, prevLabel, midiInfo;
    juce::Label lookTitle, lightNote, tapFlashLabel, tapFlashNote;
    juce::Label updatesTitle, updateNote, versionLabel;
    juce::ToggleButton spread, lightTheme, autoUpdate, isolate;
    juce::TextButton respread, checkUpdates, changelog;
    juce::ComboBox threads, spin, tapFlash;
    juce::Slider nextCC, prevCC;
};

SettingsWindow::SettingsWindow (AudioEngine& e, PluginManager& pm, Session& se, std::function<void (bool)> onThemeChanged,
                                std::function<void()> onCheckUpdates)
    : juce::DocumentWindow ("Settings", theme::bg, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new SettingsContent (e, pm, se, std::move (onThemeChanged), std::move (onCheckUpdates)), true);
    setResizable (true, false);
    setResizeLimits (600, 360, 1600, 2400);

    // never taller or wider than the screen it opens on
    auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                    ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userBounds.toNearestInt()
                    : juce::Rectangle<int> (0, 0, 1280, 800);
    centreWithSize (juce::jmin (getWidth(), area.getWidth() - 40), juce::jmin (getHeight(), area.getHeight() - 60));
}

void SettingsWindow::showPage (int page)
{
    if (auto* c = dynamic_cast<SettingsContent*> (getContentComponent()))
        c->showPage (juce::jlimit (0, SettingsContent::numPages - 1, page));
}

} // namespace stagerack
