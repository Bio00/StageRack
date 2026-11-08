#include <JuceHeader.h>
#include "UI/MainComponent.h"
#include "Bench.h"
#include "EditTest.h"
#include "UI/PluginMeters.h"
#include "UI/PluginBrowser.h"
#include "Engine/Rack.h"
#include "Update/Updater.h"

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
        setResizeLimits (1100, 560, 10000, 10000);
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

        if (args.contains ("--spread-test"))
        {
            juce::Process::setDockIconVisible (false);
            setApplicationReturnValue (runSpreadTest (args));
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
        mainWindow = nullptr;
        update::runPendingInstall();   // "Install and restart": the installer waits for this process to exit
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

START_JUCE_APPLICATION (stagerack::StageRackApplication)
