#pragma once
// --session-check <file.stagerack> [--seconds 90]
// Compatibility check of an existing session with the current build: loads it like the app does
// (real plugins, on the message loop, no audio device), waits for the plugins, then serializes it
// again and compares with the file: every attribute that was in the file must come back with the
// same value (racks, plugins per slot, scenes, top level). New attributes are listed, not errors.
// The file is never written. Exits with _Exit (plugins are not destroyed, plugins.xml not saved).
#include <JuceHeader.h>
#include "Engine/Session.h"
#include "Plugins/PluginManager.h"

namespace stagerack
{

class SessionCheck final : private juce::Timer
{
public:
    explicit SessionCheck (const juce::StringArray& a) : args (a) {}

    int start()
    {
        file = juce::File (args[args.indexOf ("--session-check") + 1]);
        if (const auto si = args.indexOf ("--seconds"); si >= 0) seconds = args[si + 1].getIntValue();

        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr) { std::cout << "cannot read " << file.getFullPathName() << std::endl; return 1; }
        original = juce::ValueTree::fromXml (*xml);

        session.loadFromTree (original.createCopy(), file, false);
        std::cout << "loading " << file.getFileName() << " (" << original.getChildWithName ("RACKS").getNumChildren() << " racks)" << std::endl;
        startTimer (250);
        return 0;
    }

private:
    const juce::StringArray args;
    juce::File file;
    juce::ValueTree original;
    int seconds = 90, ticks = 0, problems = 0;

    AudioEngine engine;
    PluginManager plugins;
    Session session { engine, plugins };

    static bool sameValue (const juce::var& a, const juce::var& b)
    {
        const auto sa = a.toString(), sb = b.toString();
        if (sa == sb) return true;
        if (sa.containsOnly ("-0123456789.e") && sb.containsOnly ("-0123456789.e"))
            return std::abs (sa.getDoubleValue() - sb.getDoubleValue()) < 1.0e-4 * juce::jmax (1.0, std::abs (sa.getDoubleValue()));
        return false;
    }

    void compare (const juce::ValueTree& was, const juce::ValueTree& now, const juce::String& path)
    {
        if (! now.isValid()) { report (path + ": missing"); return; }

        for (int i = 0; i < was.getNumProperties(); ++i)
        {
            const auto name = was.getPropertyName (i);
            if (name.toString() == "state") continue;   // plugin chunk: the plugin may re-encode it
            if (juce::StringArray { "infoUpdateTime", "fileTime", "numInputs", "numOutputs", "lastFileModTime", "lastInfoUpdateTime" }
                    .contains (name.toString()))
                continue;                                // plugin description refreshed by the latest scan
            if (! now.hasProperty (name))                       report (path + " @" + name.toString() + ": dropped");
            else if (! sameValue (was[name], now[name]))        report (path + " @" + name.toString() + ": " + was[name].toString() + " -> " + now[name].toString());
        }

        for (int i = 0; i < now.getNumProperties(); ++i)
            if (! was.hasProperty (now.getPropertyName (i)))
                added.insert (now.getType().toString() + "@" + now.getPropertyName (i).toString());

        // children by type + position among the same type
        std::map<juce::String, int> seen;
        for (const auto& c : was)
        {
            const auto type = c.getType().toString();
            const int k = seen[type]++;
            juce::ValueTree match;
            int j = 0;
            for (const auto& d : now)
                if (d.getType().toString() == type && j++ == k) { match = d; break; }
            compare (c, match, path + "/" + type + "[" + juce::String (k) + "]");
        }
    }

    void report (const juce::String& s)
    {
        ++problems;
        if (problems <= 40) std::cout << "  DIFF " << s << std::endl;
    }

    std::set<juce::String> added;

    void timerCallback() override
    {
        ++ticks;
        const bool done = ticks > 4 && session.getPendingLoads() == 0;
        if (! done && ticks < seconds * 4) return;
        stopTimer();

        if (! done) std::cout << "timeout: " << session.getPendingLoads() << " plugins still loading" << std::endl;

        int slots = 0, loaded = 0;
        for (int r = 0; r < engine.getNumRacks(); ++r)
            for (int s = 0; s < Rack::maxSlots; ++s)
                if (engine.getRack (r).getPlugin (s) != nullptr) ++loaded;
        for (const auto& rack : original.getChildWithName ("RACKS"))
            for (const auto& p : rack)
                if (p.hasType ("PLUGIN")) ++slots;
        std::cout << "plugins: " << loaded << " loaded / " << slots << " in the file" << std::endl;
        if (loaded != slots) ++problems;

        compare (original, session.toTree(), "STAGERACK");
        if (! added.empty())
        {
            std::cout << "  new attributes (ok):";
            for (auto& a : added) std::cout << " " << a;
            std::cout << std::endl;
        }

        std::cout << (problems == 0 ? "SESSION OK" : juce::String (problems) + " DIFFERENCES") << std::endl;
        std::fflush (nullptr);
        std::_Exit (problems == 0 ? 0 : 1);
    }
};

} // namespace stagerack
