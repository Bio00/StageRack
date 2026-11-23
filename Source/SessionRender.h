#pragma once
// --render-session <file.stagerack> <dir> [--height 860] [--seconds 60]
// Draws the racks of a real session (real plugins, no audio device) in vertical strips, at the
// sizes saved in the session, dark and light: session-<dark|light>.png. For checking layouts with
// the user's own sessions (thin racks, many plugins and sends). Never writes the session.
// Exits with _Exit (plugins are not destroyed).
#include <JuceHeader.h>
#include "Engine/Session.h"
#include "Plugins/PluginManager.h"
#include "UI/RackComponent.h"
#include "UI/EditController.h"
#include "UI/Theme.h"

namespace stagerack
{

class SessionRender final : private juce::Timer
{
public:
    explicit SessionRender (const juce::StringArray& a) : args (a) {}

    int start()
    {
        const auto at = args.indexOf ("--render-session");
        file = juce::File (args[at + 1]);
        dir = juce::File (args[at + 2]);
        if (const auto i = args.indexOf ("--height"); i >= 0) height = args[i + 1].getIntValue();
        if (const auto i = args.indexOf ("--seconds"); i >= 0) seconds = args[i + 1].getIntValue();
        dir.createDirectory();

        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr) { std::cout << "cannot read " << file.getFullPathName() << std::endl; return 1; }

        engine.prepare (48000.0, 64, {});
        session.loadFromTree (juce::ValueTree::fromXml (*xml), file, false);
        startTimer (250);
        return 0;
    }

private:
    void timerCallback() override
    {
        if (session.getPendingLoads() > 0 && ++ticks < seconds * 4)
            return;

        stopTimer();

        for (bool light : { false, true })
        {
            theme::setLight (light);
            theme::LookAndFeel lnf;
            juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

            juce::Component window;
            juce::OwnedArray<RackComponent> comps;
            int x = 0;

            for (int i = 0; i < engine.getNumRacks(); ++i)
            {
                if (session.isHiddenInFolder (engine.getRack (i)))
                    continue;

                auto* rc = comps.add (new RackComponent (session, edit, engine.getRack (i), i));
                rc->setVertical (true);
                const int w = rc->getPreferredLength();
                rc->setBounds (x, 0, w, height);
                x += w + 6;
            }

            window.setBounds (-30000, 0, juce::jmax (1, x), height);   // on the desktop, off screen: slots only update while showing
            window.addToDesktop (juce::ComponentPeer::windowIsTemporary);
            window.setVisible (true);
            for (auto* rc : comps)
            {
                window.addAndMakeVisible (rc);
                rc->refresh();
                rc->resized();
                for (int k = 0; k < 3; ++k) rc->updateMeters();   // slots build their rows on the stats tick
            }

            juce::Image img (juce::Image::ARGB, window.getWidth(), height, true);
            juce::Graphics g (img);
            g.fillAll (theme::bg);
            window.paintEntireComponent (g, true);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (dir.getChildFile (juce::String ("session-") + (light ? "light.png" : "dark.png")));
            os.setPosition (0); os.truncate();
            png.writeImageToStream (img, os);
            juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        }

        std::cout << "rendered " << engine.getNumRacks() << " racks to " << dir.getFullPathName() << std::endl;
        std::_Exit (0);
    }

    const juce::StringArray args;
    juce::File file, dir;
    int height = 860, seconds = 60, ticks = 0;

    AudioEngine engine;
    PluginManager plugins;
    Session session { engine, plugins };
    EditController edit { session };
};

} // namespace stagerack
