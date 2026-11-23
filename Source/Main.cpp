#include <JuceHeader.h>
#include "UI/MainComponent.h"
#include "Bench.h"
#include "PluginProbe.h"
#include "TempoProbe.h"
#include "WatchTest.h"
#include "SoakTest.h"
#include "SandboxTest.h"
#include "Plugins/Sandbox/SandboxHost.h"
#include "SessionCheck.h"
#include "SessionRender.h"
#include "EditTest.h"
#include "EditorTest.h"
#include "UndoTest.h"
#include "DeviceGuardTest.h"
#include "StressTest.h"
#include "AutomixProbe.h"
#include "UI/PluginMeters.h"
#include "UI/PluginBrowser.h"
#include "UI/TopBar.h"
#include "UI/PluginManagerWindow.h"
#include "UI/AutomixerWindow.h"
#include "UI/TempoWindow.h"
#include "UI/EditGroupsWindow.h"
#include "UI/MidiMapOverlay.h"
#include "UI/MidiWindow.h"
#include "MidiLoopTest.h"
#include "Engine/Rack.h"
#include "Update/Updater.h"
#include "Update/Changelog.h"
#include "UI/ChangelogWindow.h"
#include "Engine/CoreAudioProbe.h"

namespace stagerack
{

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow()
        : juce::DocumentWindow ("StageRack", theme::bg, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent(), true);
        setBackgroundColour (theme::bg);   // the saved theme is applied by MainComponent
        setResizable (true, false);
        setResizeLimits (1160, 560, 10000, 10000);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    MainComponent& getMain() { return *static_cast<MainComponent*> (getContentComponent()); }
};

class StageRackApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return true; } // scanner child processes

    void initialise (const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();

        if (sandbox::isSandboxCommandLine (getCommandLineParameters()))
        {
            // EXPERIMENTAL plugin sandbox child: serves one plugin to the StageRack that launched it
            if (! sandbox::startSandboxChild (getCommandLineParameters()))
                std::_Exit (1);
            return;
        }

        if (args.contains (coreaudio::probeArgument))
        {
            // CoreAudio probe child (see CoreAudioProbe.h): one call to coreaudiod, exit.
            juce::Process::setDockIconVisible (false);
            std::_Exit (coreaudio::runProbeChild());
        }

        if (args.contains (PluginManager::scanArgument))
        {
            // Plugin scanner child process: no UI, scan one file, exit.
            juce::Process::setDockIconVisible (false);
            const auto result = PluginManager::runScannerChild (args);

            // Leave immediately: skip unloading the plugin (and its static destructors).
            std::fflush (nullptr);
            std::_Exit (result);
        }

        if (args.contains (PluginManager::latencyArgument))
        {
            // Latency probe child: load one plugin, report its latency, exit without unloading.
            juce::Process::setDockIconVisible (false);
            const auto result = PluginManager::runLatencyProbeChild (args);
            std::fflush (nullptr);
            std::_Exit (result);
        }

        if (args.contains ("--whats-new") || args.contains ("--render-changelog"))
        {
            // Diagnostics: --whats-new [seenVersion]  prints the notes the app would show after an
            // update from seenVersion (none = whole history); --render-changelog <dir> [seenVersion]
            // also draws the window (dark + light) to PNG
            juce::Process::setDockIconVisible (false);
            const bool render = args.contains ("--render-changelog");
            const int at = args.indexOf (render ? "--render-changelog" : "--whats-new");
            const juce::File dir (render ? args[at + 1] : juce::String());
            const auto seen = args[at + (render ? 2 : 1)].startsWith ("-") ? juce::String() : args[at + (render ? 2 : 1)];
            const auto md = update::bundledChangelog();
            const auto entries = update::parseChangelog (md);
            std::printf ("current %s, %d versions in the bundled changelog\n", update::currentVersion().toRawUTF8(), entries.size());
            const auto notes = seen.isNotEmpty() ? update::changesBetween (md, seen, update::currentVersion()) : md;
            std::printf ("%s\n", notes.toRawUTF8());

            if (render)
            {
                for (bool light : { false, true })
                {
                    theme::setLight (light);
                    theme::LookAndFeel lnf;
                    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
                    auto img = ChangelogWindow::render (notes, 620, 640);
                    juce::PNGImageFormat png;
                    juce::FileOutputStream out (dir.getChildFile (light ? "changelog-light.png" : "changelog-dark.png"));
                    out.setPosition (0); out.truncate();
                    png.writeImageToStream (img, out);
                    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
                }
            }

            std::fflush (stdout);
            std::_Exit (md.isNotEmpty() && ! entries.isEmpty() ? 0 : 1);
        }

        if (args.contains ("--render-browser"))
        {
            // Diagnostics: --render-browser <dir>  draws the plugin browser (dark + light) to PNG, no audio device
            juce::Process::setDockIconVisible (false);
            const juce::File dir (args[args.indexOf ("--render-browser") + 1]);
            PluginManager pm;

            if (args.contains ("--fake-latencies"))   // shows the dot colours; writes latency.xml, delete it afterwards
            {
                auto types = pm.getKnownList().getTypes();
                std::sort (types.begin(), types.end(), [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
                const int samples[] = { 0, 48, 960, 0, 2400, -1 };
                for (int i = 0; i < 6 && i < types.size(); ++i)
                    if (samples[i] >= 0) pm.recordLatency (types.getReference (i), samples[i], 48000.0);
            }

            for (bool light : { false, true })
            {
                theme::setLight (light);
                theme::LookAndFeel lnf;
                juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
                {
                    PluginBrowser b (pm, [] (const juce::PluginDescription&) {});
                    juce::Image img (juce::Image::ARGB, b.getWidth(), b.getHeight(), true);
                    juce::Graphics g (img);
                    g.fillAll (theme::panel);
                    b.paintEntireComponent (g, true);
                    juce::PNGImageFormat png;
                    juce::FileOutputStream out (dir.getChildFile (light ? "browser-light.png" : "browser-dark.png"));
                    out.setPosition (0); out.truncate();
                    png.writeImageToStream (img, out);
                }
                juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
            }

            std::fflush (nullptr);
            std::_Exit (0);
        }

        if (args.contains ("--render-settings"))
        {
            // Diagnostics: --render-settings <dir>  draws every page of the Settings window (PNG; use STAGERACK_DATA_DIR: it writes settingsPage)
            juce::Process::setDockIconVisible (false);
            const juce::File dir (args[args.indexOf ("--render-settings") + 1]);
            AudioEngine engine;
            auto* pm = new PluginManager();          // leaked on purpose: its destructor saves files
            Session session (engine, *pm);
            theme::LookAndFeel lnf;
            juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
            engine.getDeviceManager().initialise (2, 2, nullptr, true);
            auto* w = new SettingsWindow (engine, *pm, session, [] (bool) {}, [] {});   // leaked: _Exit below
            auto* content = w->getContentComponent();
            for (int page = 0; page < 6; ++page)   // settings-0.png ... settings-5.png
            {
                w->showPage (page);
                juce::Image img (juce::Image::ARGB, content->getWidth(), content->getHeight(), true);
                juce::Graphics g (img);
                content->paintEntireComponent (g, true);
                juce::PNGImageFormat png;
                juce::FileOutputStream os (dir.getChildFile ("settings-" + juce::String (page) + ".png"));
                os.setPosition (0); os.truncate();
                png.writeImageToStream (img, os);
            }
            std::fflush (nullptr);
            std::_Exit (0);
        }

        if (args.contains ("--session-roundtrip"))
        {
            // Diagnostics: --session-roundtrip <in.stagerack> <out.xml>  loads a session (no device;
            // plugins are not created) and writes it back with the current code, to compare with old files
            juce::Process::setDockIconVisible (false);
            const auto i = args.indexOf ("--session-roundtrip");
            const juce::File in (args[i + 1]), out (args[i + 2]);
            AudioEngine engine;
            auto* pm = new PluginManager();   // leaked on purpose (its destructor saves files)
            Session session (engine, *pm);
            const auto tree = juce::ValueTree::fromXml (in.loadFileAsString());
            session.loadFromTree (tree, in, false);
            std::printf ("racks %d, routing %s\n", engine.getNumRacks(), engine.isRoutingEnabled() ? "on" : "off");
            for (int r = 0; r < engine.getNumRacks(); ++r)
            {
                auto& rk = engine.getRack (r);
                std::printf ("  %-16s in %d%s out %d%s gain %.1f bus %d outBus %d stage %d folder %d\n", rk.name.toRawUTF8(),
                             rk.inputChannel.load(), rk.stereoInput.load() ? "st" : "m", rk.outputChannel.load(), rk.stereoOutput.load() ? "st" : "m",
                             rk.gainDb.load(), rk.busIndex.load(), rk.outputBus.load(), rk.stage, rk.folderId);
            }
            out.replaceWithText (session.toTree().toXmlString());
            std::fflush (nullptr);
            std::_Exit (0);
        }

        if (args.contains ("--midi-fake-midimix"))
        {
            runFakeMidiMix (juce::File (args[args.indexOf ("--midi-fake-midimix") + 1]));
            return;
        }

        if (args.contains ("--midi-loop-test"))
        {
            juce::Process::setDockIconVisible (false);
            std::thread ([] { runMidiLoopTest(); }).detach();   // the message loop keeps running
            return;
        }

        if (args.contains ("--render-racks"))
        {
            // Diagnostics: --render-racks <dir>  draws racks with the automixer at work + the top bar (PNG, no device)
            juce::Process::setDockIconVisible (false);
            const juce::File dir (args[args.indexOf ("--render-racks") + 1]);
            AudioEngine engine;
            auto* pm = new PluginManager();          // leaked on purpose: its destructor saves files
            Session session (engine, *pm);
            EditController edit (session);

            const char* names[] = { "Pastore", "Lettore", "Coro", "Musica" };
            for (int i = 0; i < 4; ++i)
            {
                auto& r = engine.addRack();
                r.name = names[i];
                r.inputChannel = i; r.stereoInput = false; r.outputChannel = 0;
                r.automixGroup = i < 3 ? 1 : 0;
            }
            engine.getRack (2).automixWeightDb = -6.0f;
            engine.getRack (0).editGroup = engine.getRack (1).editGroup = 1;   // group A active
            engine.getRack (2).editGroup = 2;                                   // group B suspended
            session.setEditGroupActive (2, false);

            // --routing: Coro = Bus B (sub-group of the two voices), Musica = Bus A (FX return)
            const bool routingDemo = args.contains ("--routing");
            if (routingDemo)
            {
                engine.setRoutingEnabled (true);
                engine.getRack (3).name = "Reverb"; engine.getRack (3).inputChannel = -1;
                engine.getRack (2).name = "Sub voci"; engine.getRack (2).inputChannel = -1; engine.getRack (2).automixGroup = 0;
                engine.setRackBus (engine.getRack (3), 1);
                engine.setRackBus (engine.getRack (2), 2);
                engine.getRack (0).sends[0].db = -6.0f;
                engine.getRack (1).sends[0].db = -14.0f;
                engine.getRack (1).sends[0].pre = true;
                engine.getRack (2).sends[0].db = -20.0f;
                engine.setRackOutputBus (engine.getRack (0), 2);
                engine.setRackOutputBus (engine.getRack (1), 2);
                // multiple outputs: the voice also goes to the monitor pair, the reverb to two more
                engine.setRackExtraOutputs (engine.getRack (0), { Rack::outStereo + 4 });
                engine.setRackExtraOutputs (engine.getRack (3), { Rack::outStereo + 2, Rack::outMono + 7 });
            }

            // --view 1: no input, no info, sends of Bus A only (Lettore keeps all: its own choice)
            // --view 2: no inserts (sends get the room), no I/O selectors
            const int viewDemo = args.contains ("--view") ? args[args.indexOf ("--view") + 1].getIntValue() : 0;
            if (viewDemo == 1)
            {
                Session::RackView v;
                v.input = false; v.info = false; v.sendsMask = 1;
                session.setRackView (v);
                engine.getRack (1).uiSendsShown = 0xff;
                engine.getRack (0).uiHideSections = 1 | 2 | 4 | 16;   // Pastore: only its sends
            }
            else if (viewDemo == 2)
            {
                Session::RackView v;
                v.inserts = false; v.io = false;
                session.setRackView (v);
            }

            engine.prepare (48000.0, 64, {});

            // tempos: master tapped at 96, T1 own (vocal delay, key Y + footswitch CC 64), T2 = master x2,
            // T3 tracking a kick on IN 4; the music rack plays with T1
            {
                auto& tempo = engine.tempo;
                for (int k = 0; k < 3; ++k) tempo.addTempo();
                const auto now = TempoEngine::nowSec();
                for (int k = 0; k < 4; ++k)
                    tempo.tap (0, now - 1.9 + k * 0.625, TempoEngine::srcButton);
                auto c1 = tempo.getConfig (1);
                c1.name = "Vox delay"; c1.keyCode = 'Y'; c1.midiKind = TempoEngine::midiCC; c1.midiNumber = 64; c1.tapValue = TempoEngine::tapEighth;
                tempo.setConfig (1, c1);
                tempo.setBpm (1, 72.5);
                tempo.link (2, 2.0);
                auto c2 = tempo.getConfig (2);   // audio from a rack, after its plugins; back to the audio after 8 s
                c2.audioPoint = TempoEngine::pointPostPlugins; c2.audioRackId = engine.getRack (3).getId(); c2.returnToAudioSec = 8; c2.audioWindowPct = 6.0f; c2.oscOut = true; c2.oscOutHost = "192.168.1.50"; c2.midiOutId = "x"; c2.midiOutName = "IAC Bus 1"; c2.midiOutKind = TempoEngine::midiNote; c2.midiOutClock = true;
                tempo.setConfig (2, c2);
                auto c3 = tempo.getConfig (3);
                c3.name = "Drums"; c3.audioChannel = 3; c3.audioMode = TempoEngine::audioFollow; c3.midiClock = true;
                tempo.setConfig (3, c3);
                auto c0 = tempo.getConfig (0);
                c0.midiKind = TempoEngine::midiNote; c0.midiNumber = 36; c0.midiChannel = 10;
                tempo.setConfig (0, c0);
                engine.getRack (3).tempoIndex = 1;
                engine.getRack (3).latencyGroup = 1;   // tempo + latency group + automix on the same racks: all badges visible
                engine.getRack (0).latencyGroup = 1;
                engine.getRack (0).tempoIndex = 2;
            }

            // a few real plugins (Apple's), so the slots show names and parameters
            for (auto [rackIndex, slot, id] : { std::tuple { 0, 0, "AudioUnit:Effects/aufx,dely,appl" },
                                                std::tuple { 0, 1, "AudioUnit:Effects/aufx,dcmp,appl" },
                                                std::tuple { 1, 0, "AudioUnit:Effects/aufx,dcmp,appl" },
                                                std::tuple { 3, 2, "AudioUnit:Effects/aufx,dely,appl" } })
            {
                juce::OwnedArray<juce::PluginDescription> found;
                for (auto* f : pm->getFormatManager().getFormats())
                    if (f->getName() == "AudioUnit")
                        f->findAllTypesForFile (found, id);
                juce::String err;
                if (! found.isEmpty())
                    if (auto p = pm->getFormatManager().createPluginInstance (*found[0], 48000.0, 64, err))
                    {
                        Rack::configurePlugin (*p, 48000.0, 64);
                        engine.getRack (rackIndex).setPlugin (slot, std::move (p));
                    }
            }

            // --midi: a MIDImix (quick setup on the bank), map mode on, the bank strip 2 gain selected
            const bool midiDemo = args.contains ("--midi");
            std::unique_ptr<MidiControl> midiCtl;
            if (midiDemo)
            {
                midiCtl = std::make_unique<MidiControl> (session);
                midiCtl->setControllerEnabled ("demo-midimix", "MIDI Mix", true);
                midiCtl->quickSetup ("demo-midimix");
                MidiMapping m;
                m.device = "demo-midimix"; m.deviceName = "MIDI Mix";
                m.source = { MidiSource::cc, 1, 62 };
                m.target = MidiTarget::forRack (MidiTarget::pluginParam, MidiTarget::fixedRack, engine.getRack (0).getId(), 0, 0);
                if (auto* p = engine.getRack (0).getPlugin (0)) m.target.paramName = p->getParameters()[0]->getName (64);
                midiCtl->addMapping (m);
                for (auto [n, v] : { std::pair { 19, 90 }, std::pair { 23, 40 }, std::pair { 62, 64 }, std::pair { 10, 0 } })
                    midiCtl->injectForTests ("demo-midimix", juce::MidiMessage::controllerEvent (1, n, v));
                midiCtl->injectForTests ("demo-midimix", juce::MidiMessage::noteOn (1, 4, (juce::uint8) 127));
                midiCtl->setMapMode (true);
                midiCtl->setLearnTarget (MidiTarget::forRack (MidiTarget::rackGain, MidiTarget::bankRack, 1));
            }

            // rack 1 talks loud, rack 2 softer, rack 3 only noise
            juce::AudioBuffer<float> in (4, 64), out (2, 64);
            juce::Random rnd (3);
            juce::AudioIODeviceCallbackContext ctx;
            for (int k = 0; k < 400; ++k)
            {
                for (int i = 0; i < 64; ++i)
                {
                    in.setSample (0, i, 0.3f * (rnd.nextFloat() - 0.5f));
                    in.setSample (1, i, 0.1f * (rnd.nextFloat() - 0.5f));
                    in.setSample (2, i, 0.01f * (rnd.nextFloat() - 0.5f));
                    in.setSample (3, i, 0.2f * (rnd.nextFloat() - 0.5f));
                }
                engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 4, out.getArrayOfWritePointers(), 2, 64, ctx);
            }

            for (bool light : { false, true })
            {
                theme::setLight (light);
                theme::LookAndFeel lnf;
                juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

                // horizontal, vertical, short horizontal, narrow vertical, thinnest vertical, vertical with
                // each rack's dividers moved (only sends / only plugins / big input / big knobs)
                for (int variant : { 0, 1, 2, 3, 4, 5 })
                {
                    const bool vertical = variant != 0 && variant != 2;
                    const float size = variant == 4 ? EditController::minRackSize : variant == 2 ? 0.5f : variant == 3 ? 0.55f : 1.0f;
                    for (int i = 0; i < 4; ++i)
                    {
                        auto& rk = engine.getRack (i);
                        rk.uiSize = size;
                        rk.uiSendsV = variant == 5 ? std::array<float, 4> { 2000.0f, 0.0f, -1.0f, 300.0f }[(size_t) i] : -1.0f;
                        rk.uiInputV = variant == 5 && i >= 2 ? 120.0f : -1.0f;
                        rk.uiOutputV = variant == 5 && i >= 1 ? 230.0f : (variant == 4 && i == 3 ? 200.0f : -1.0f);
                    }
                    const int rw = juce::roundToInt (RackComponent::baseWidth * size), rh = juce::roundToInt (RackComponent::baseHeight * size);
                    juce::Component window;
                    const int w = vertical ? 4 * rw : 1400, h = vertical ? 700 : 4 * rh + 76;
                    window.setBounds (0, 0, w, h);
                    TopBar bar (session);
                    if (! vertical) { bar.setBounds (0, 0, w, TopBar::preferredHeight); window.addAndMakeVisible (bar); }
                    juce::OwnedArray<RackComponent> comps;

                    for (int i = 0; i < 4; ++i)
                    {
                        auto* rc = comps.add (new RackComponent (session, edit, engine.getRack (i), i));
                        rc->setVertical (vertical);
                        rc->setBounds (vertical ? juce::Rectangle<int> (i * rw, 0, rw, h)
                                                : juce::Rectangle<int> (0, TopBar::preferredHeight + i * rh, w, rh));
                        window.addAndMakeVisible (rc);
                        rc->updateMeters();
                    }
                    bar.updateMeters();

                    std::unique_ptr<MidiMapOverlay> overlay;
                    if (midiDemo && variant > 1)
                        continue;
                    if (midiDemo)
                    {
                        overlay = std::make_unique<MidiMapOverlay> (window, session, *midiCtl);
                        overlay->setBounds (window.getLocalBounds());
                        window.addAndMakeVisible (*overlay);
                        overlay->setActive (true);
                        overlay->setActive (true);   // a second pass: the targets of the components laid out by the first
                    }

                    juce::Image img (juce::Image::ARGB, w, h, true);
                    juce::Graphics g (img);
                    g.fillAll (theme::bg);
                    window.paintEntireComponent (g, true);
                    juce::PNGImageFormat png;
                    juce::FileOutputStream os (dir.getChildFile (juce::String (midiDemo ? "midi-map-" : viewDemo > 0 ? "view" + juce::String (viewDemo) + "-" : routingDemo ? "routing-" : "racks-") + (vertical ? "v-" : "h-") + (variant == 4 ? "tiny-" : variant == 5 ? "dividers-" : variant >= 2 ? "small-" : "") + (light ? "light.png" : "dark.png")));
                    os.setPosition (0); os.truncate();
                    png.writeImageToStream (img, os);
                }

                for (int width : { 1160, 1300, 1700 })   // top bar at the minimum window width and wider
                {
                    TopBar bar (session);
                    bar.setBounds (0, 0, width, TopBar::preferredHeight);
                    bar.updateMeters();
                    juce::Image img (juce::Image::ARGB, width, TopBar::preferredHeight, true);
                    juce::Graphics g (img);
                    bar.paintEntireComponent (g, true);
                    juce::PNGImageFormat png;
                    juce::FileOutputStream os (dir.getChildFile ("topbar-" + juce::String (width) + (light ? "-light.png" : "-dark.png")));
                    os.setPosition (0); os.truncate();
                    png.writeImageToStream (img, os);
                }

                // customised (user layout): device first, snapshots last, session hidden; then with
                // the automixer, MIDI and edit groups switched off in Features; then in edit mode
                for (int demo = 0; demo < 3; ++demo)
                {
                    if (demo >= 1)
                        for (auto f : { features::automix, features::midi, features::editGroups })
                            features::enabled[f] = false;

                    TopBar bar (session);
                    bar.setLayoutString ("device,dsp,mute,automix,groups,tempoWindow,midi,tempos,pluginsSettings,view,layoutTheme,snapshots,-session");
                    bar.setBounds (0, 0, 1300, TopBar::preferredHeight);
                    if (demo == 2) bar.setCustomizing (true);
                    bar.updateMeters();
                    juce::Image img (juce::Image::ARGB, 1300, TopBar::preferredHeight, true);
                    juce::Graphics g (img);
                    bar.paintEntireComponent (g, true);
                    juce::PNGImageFormat png;
                    juce::FileOutputStream os (dir.getChildFile ("topbar-custom" + juce::String (demo) + (light ? "-light.png" : "-dark.png")));
                    os.setPosition (0); os.truncate();
                    png.writeImageToStream (img, os);
                    bar.setCustomizing (false);
                }

                for (int f = 0; f < features::numFeatures; ++f)
                    features::enabled[f] = true;

                if (midiDemo)
                {
                    MidiPanel panel (session, *midiCtl);
                    panel.setSize (1240, 760);
                    panel.refreshAll();
                    juce::Image img (juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true);
                    juce::Graphics g (img);
                    panel.paintEntireComponent (g, true);
                    juce::PNGImageFormat png;
                    juce::FileOutputStream os (dir.getChildFile (light ? "midi-window-light.png" : "midi-window-dark.png"));
                    os.setPosition (0); os.truncate();
                    png.writeImageToStream (img, os);
                }

                {
                    AutomixerPanel panel (session);
                    panel.setSize (480, 440);
                    panel.tick();
                    juce::Image img (juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true);
                    juce::Graphics g (img);
                    panel.paintEntireComponent (g, true);
                    juce::PNGImageFormat png;
                    juce::FileOutputStream os (dir.getChildFile (light ? "automixer-light.png" : "automixer-dark.png"));
                    os.setPosition (0); os.truncate();
                    png.writeImageToStream (img, os);
                }

                {
                    TempoPanel panel (session);
                                        panel.tick();
                    juce::Image img (juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true);
                    juce::Graphics g (img);
                    panel.paintEntireComponent (g, true);
                    juce::PNGImageFormat png;
                    juce::FileOutputStream os (dir.getChildFile (light ? "tempo-light.png" : "tempo-dark.png"));
                    os.setPosition (0); os.truncate();
                    png.writeImageToStream (img, os);
                }

                {
                    auto send = TempoPanel::createSendPanel (session, 2);
                    juce::Image img (juce::Image::ARGB, send->getWidth(), send->getHeight(), true);
                    juce::Graphics g (img);
                    g.fillAll (theme::panel);
                    send->paintEntireComponent (g, true);
                    juce::PNGImageFormat png;
                    juce::FileOutputStream os (dir.getChildFile (light ? "tempo-send-light.png" : "tempo-send-dark.png"));
                    os.setPosition (0); os.truncate();
                    png.writeImageToStream (img, os);
                }

                {
                    edit.clickRack (engine.getRack (3).getId(), {});
                    EditGroupsPanel panel (session, edit);
                    panel.resized();
                    juce::Image img (juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true);
                    juce::Graphics g (img);
                    panel.paintEntireComponent (g, true);
                    juce::PNGImageFormat png;
                    juce::FileOutputStream os (dir.getChildFile (light ? "groups-light.png" : "groups-dark.png"));
                    os.setPosition (0); os.truncate();
                    png.writeImageToStream (img, os);
                }


                juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
            }

            std::fflush (nullptr);
            std::_Exit (0);
        }

        if (args.contains ("--meters"))
        {
            // Diagnostics: --meters <description.xml>  lists the parameters shown in the slot's meter view
            juce::Process::setDockIconVisible (false);
            juce::PluginDescription desc;
            int rc = 1;

            if (auto xml = juce::parseXML (juce::File (args[args.indexOf ("--meters") + 1])); xml != nullptr && desc.loadFromXml (*xml))
            {
                juce::AudioPluginFormatManager fm;
                PluginManager::addAllFormats (fm);
                juce::String err;

                if (auto p = fm.createPluginInstance (desc, 48000.0, 64, err))
                {
                    Rack::configurePlugin (*p, 48000.0, 64);
                    const auto meters = findMeters (*p);
                    std::printf ("%s: %d parameters, %d meters\n", desc.name.toRawUTF8(), p->getParameters().size(), meters.size());

                    for (auto& m : meters)
                        std::printf ("  %s%s = %s (%.2f)\n", m.name.toRawUTF8(), m.gainReduction ? " [GR]" : "",
                                     meterText (*p, m).toRawUTF8(), (double) readMeter (*p, m));
                    rc = 0;
                }
            }

            std::fflush (nullptr);
            std::_Exit (rc);
        }

        if (args.contains ("--check-update"))
        {
            // Prints what the updater sees on GitHub; with --download also downloads and verifies.
            juce::Process::setDockIconVisible (false);
            juce::String error;
            const auto rel = update::fetchLatest (error);
            int rc = 1;

            std::printf ("current %s\n", update::currentVersion().toRawUTF8());

            if (! rel.has_value())
            {
                std::printf ("error: %s\n", error.toRawUTF8());
            }
            else
            {
                const auto newer = update::compareVersions (rel->version, update::currentVersion()) > 0;
                std::printf ("latest %s (%s%s) %s, %lld bytes\n%s\n", rel->version.toRawUTF8(), rel->tag.toRawUTF8(),
                             rel->prerelease ? ", pre-release" : "", rel->assetName.toRawUTF8(), (long long) rel->assetSize,
                             newer ? "UPDATE AVAILABLE" : "up to date");
                if (rel->changelog.isNotEmpty())
                    std::printf ("what's new: %d versions, %d chars (first: %s)\n",
                                 update::parseChangelog (rel->changelog).size(),
                                 rel->changelog.length(), rel->changelog.upToFirstOccurrenceOf ("\n", false, false).toRawUTF8());
                rc = 0;

                if (args.contains ("--download"))
                {
                    const auto zip = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("stagerack-update-test/" + rel->assetName);
                    int last = -1;
                    auto err = update::download (*rel, zip, [&last] (double p) { if ((int) (p * 10) != last) { last = (int) (p * 10); std::printf ("  %d%%\n", last * 10); } }, {});
                    juce::File app;
                    if (err.isEmpty())
                        app = update::unpackAndVerify (zip, *rel, err);
                    std::printf ("%s\n", err.isEmpty() ? ("verified: " + app.getFullPathName()).toRawUTF8() : ("FAILED: " + err).toRawUTF8());
                    rc = err.isEmpty() ? 0 : 1;
                }
            }

            std::fflush (nullptr);
            std::_Exit (rc);
        }

        if (args.contains ("--edit-selftest"))
        {
            juce::Process::setDockIconVisible (false);
            runEditSelfTest();   // exits
        }

        if (args.contains ("--automix-probe"))
        {
            juce::Process::setDockIconVisible (false);
            automixprobe::run (args);   // exits
        }

        if (args.contains ("--selftest"))
        {
            juce::Process::setDockIconVisible (false);
            setApplicationReturnValue (runSelfTest());
            quit();
            return;
        }

        if (args.contains ("--handover-test"))
        {
            juce::Process::setDockIconVisible (false);
            setApplicationReturnValue (runHandoverTest (args));
            quit();
            return;
        }

        if (args.contains ("--stress-lifecycle"))
        {
            juce::Process::setDockIconVisible (false);
            static std::unique_ptr<LifecycleStress> stress;   // drives itself on the message loop, exits with _Exit
            stress = std::make_unique<LifecycleStress> (args);
            if (stress->start() != 0) std::_Exit (1);
            return;
        }

        if (args.contains ("--spread-test"))
        {
            juce::Process::setDockIconVisible (false);
            setApplicationReturnValue (runSpreadTest (args));
            quit();
            return;
        }

        if (args.contains ("--render-session"))
        {
            juce::Process::setDockIconVisible (false);
            static std::unique_ptr<SessionRender> render;   // waits for the plugins on the message loop, exits with _Exit
            render = std::make_unique<SessionRender> (args);
            if (render->start() != 0) std::_Exit (1);
            return;
        }

        if (args.contains ("--session-check"))
        {
            juce::Process::setDockIconVisible (false);
            static std::unique_ptr<SessionCheck> check;   // drives itself on the message loop, exits with _Exit
            check = std::make_unique<SessionCheck> (args);
            if (check->start() != 0) std::_Exit (1);
            return;
        }

        if (args.contains ("--sandbox-test"))
        {
            juce::Process::setDockIconVisible (false);
            static std::unique_ptr<SandboxTest> test;   // drives itself on the message loop, exits with _Exit
            test = std::make_unique<SandboxTest> (args);
            if (test->start() != 0) std::_Exit (1);
            return;
        }

        if (args.contains ("--soak"))
        {
            juce::Process::setDockIconVisible (false);
            static std::unique_ptr<SoakTest> test;   // drives itself on the message loop, exits with _Exit
            test = std::make_unique<SoakTest> (args);
            if (test->start() != 0) std::_Exit (1);
            return;
        }

        if (args.contains ("--watch-test"))
        {
            juce::Process::setDockIconVisible (false);
            static std::unique_ptr<HopWatchTest> test;   // drives itself on the message loop, exits with _Exit
            test = std::make_unique<HopWatchTest> (args);
            if (test->start() != 0) std::_Exit (1);
            return;
        }

        if (args.contains ("--device-guard-test"))
        {
            juce::Process::setDockIconVisible (false);
            static std::unique_ptr<DeviceGuardTest> test;   // drives itself on the message loop, exits with _Exit
            test = std::make_unique<DeviceGuardTest>();
            if (test->start() != 0) std::_Exit (1);
            return;
        }

        if (args.contains ("--undo-selftest"))
        {
            juce::Process::setDockIconVisible (false);
            runUndoSelfTest();
            return;
        }

        if (args.contains ("--editor-test"))
        {
            static std::unique_ptr<EditorTest> test;   // drives itself on the message loop, exits with _Exit
            test = std::make_unique<EditorTest> (args);
            if (test->start() != 0) std::_Exit (1);
            return;
        }

        if (args.contains ("--tempo-probe"))
        {
            setApplicationReturnValue (runTempoProbe (args));
            quit();
            return;
        }

        if (args.contains ("--param-probe"))
        {
            juce::Process::setDockIconVisible (false);
            juce::MessageManager::callAsync ([args] { runParamProbe (args); });
            return;
        }
        if (args.contains ("--plugin-probe"))
        {
            juce::Process::setDockIconVisible (false);
            setApplicationReturnValue (runPluginProbe (args));
            quit();
            return;
        }

        if (args.contains ("--bench-device"))
        {
            juce::Process::setDockIconVisible (false);
            setApplicationReturnValue (runDeviceBenchmark (args));
            quit();
            return;
        }

        if (args.contains ("--bench"))
        {
            juce::Process::setDockIconVisible (false);
            setApplicationReturnValue (runBenchmark (args));
            quit();
            return;
        }

        mainWindow = std::make_unique<MainWindow>();
    }

    void shutdown() override
    {
        if (mainWindow != nullptr)
        {
            // Off the screen first: from here on nothing may repaint (the top bar reads the
            // session, which goes away), and a window in full screen would even be re-created
            // by JUCE on a look-and-feel change.
            mainWindow->removeFromDesktop();
            mainWindow->getMain().prepareToQuit (true);   // with the 5 s watchdog
        }

        update::runPendingInstall();   // "Install and restart": the installer waits for this process to exit

        // Everything is saved and the plugins are gone. Tearing down the rest (windows, plugin
        // formats, JUCE singletons) only frees memory, and a heap damaged by some plugin turns
        // exactly that into a crash at quit (seen: LV2 format temp dir after UA/PACE plugins).
        std::fflush (nullptr);
        std::_Exit (0);
    }

    void systemRequestedQuit() override
    {
        if (mainWindow == nullptr || ! mainWindow->getMain().getSession().isDirty())
        {
            quit();
            return;
        }

        juce::AlertWindow::showYesNoCancelBox (juce::MessageBoxIconType::QuestionIcon, "StageRack",
            "Save changes to the current session before quitting?", "Save", "Don't save", "Cancel", nullptr,
            juce::ModalCallbackFunction::create ([this] (int result)
            {
                if (result == 1)
                {
                    auto& main = mainWindow->getMain();

                    if (main.getSession().getFile().existsAsFile())
                    {
                        main.getSession().saveTo (main.getSession().getFile());
                        quit();
                    }
                    else
                    {
                        main.saveSession (true);
                    }
                }
                else if (result == 2)
                {
                    quit();
                }
                else
                {
                    update::clearPendingInstall();   // quit cancelled: no update either
                }
            }));
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace stagerack

JUCE_CREATE_APPLICATION_DEFINE (stagerack::StageRackApplication)

int main (int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a (argv[i]);

        if (a == stagerack::coreaudio::probeArgument)
            std::_Exit (stagerack::coreaudio::runProbeChild());   // no AppKit at all: never a Dock icon

        if (a.rfind ("--srk-sandbox:", 0) == 0)
            stagerack::markAsAccessoryProcess();   // plugin sandbox child: no Dock icon, may open the plugin's window

        if (a == stagerack::PluginManager::scanArgument || a == stagerack::PluginManager::latencyArgument || a == "--midi-fake-midimix")
            stagerack::markAsBackgroundProcess();
    }

    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main (argc, (const char**) argv);
}
