#pragma once

// "StageRack --editor-test <format> <id>": reproduces the "generic editor instead of the plugin's own"
// problem: offscreen mini-view capture, parameter changes, opening/closing the plugin window.
#include <JuceHeader.h>
#include "Plugins/PluginManager.h"
#include "Engine/Rack.h"
#include "UI/PluginWindow.h"
#include "UI/PluginThumbnail.h"

namespace stagerack
{

class EditorTest final : private juce::Timer
{
public:
    explicit EditorTest (const juce::StringArray& a) : args (a) {}

    int start()
    {
        const auto idx = args.indexOf ("--editor-test");
        PluginManager::addAllFormats (fm);
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : fm.getFormats())
            if (f->getName() == args[idx + 1])
                f->findAllTypesForFile (found, args[idx + 2]);
        if (found.isEmpty()) { std::cout << "plugin not found" << std::endl; return 1; }
        juce::String err;
        plugin = fm.createPluginInstance (*found[0], 48000.0, 64, err);
        if (plugin == nullptr) { std::cout << "create failed: " << err << std::endl; return 1; }
        Rack::configurePlugin (*plugin, 48000.0, 64);
        plugin->prepareToPlay (48000.0, 64);
        startTimer (100);
        return 0;
    }

private:
    void report (const char* what)
    {
        if (auto* ed = plugin->getActiveEditor())
            std::cout << what << ": " << PluginThumbnails::describeEditor (*ed) << std::endl;
        else
            std::cout << what << ": no active editor" << std::endl;
    }

    void tweak()
    {
        auto params = plugin->getParameters();
        for (int i = 0; i < juce::jmin (8, params.size()); ++i)
            params[i]->setValueNotifyingHost (juce::Random::getSystemRandom().nextFloat());
    }

    void timerCallback() override
    {
        ++tick;   // 100 ms per tick
        auto& th = PluginThumbnails::get();
        const juce::String title ("editor-test");

        switch (tick)
        {
            case 1:  th.requestOffscreen (*plugin); report ("offscreen requested"); break;
            case 4:  tweak(); PluginWindow::show (*plugin, title); report ("A: window opened during offscreen capture"); break;
            case 15: PluginWindow::closeFor (plugin.get()); report ("A closed"); break;
            case 16: th.requestOffscreen (*plugin); break;
            case 25: report ("B: after offscreen capture done"); PluginWindow::show (*plugin, title); report ("B: window opened"); break;
            case 30: tweak(); break;
            case 35: PluginWindow::closeFor (plugin.get()); tweak(); th.requestOffscreen (*plugin); break;
            case 36: tweak(); break;
            case 45: PluginWindow::show (*plugin, title); report ("C: reopened after tweak + capture"); break;
            case 55: PluginWindow::closeFor (plugin.get()); PluginWindow::show (*plugin, title); report ("D: close+reopen immediately"); break;
            case 65: PluginWindow::closeFor (plugin.get());
                     for (int i = 0; i < 5; ++i) { PluginWindow::show (*plugin, title); PluginWindow::closeFor (plugin.get()); }
                     PluginWindow::show (*plugin, title); report ("E: after 5 fast open/close"); break;
            case 80: PluginWindow::closeFor (plugin.get()); std::fflush (nullptr); std::_Exit (0);
            default: break;
        }
    }

    juce::StringArray args;
    juce::AudioPluginFormatManager fm;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    int tick = 0;
};

} // namespace stagerack
