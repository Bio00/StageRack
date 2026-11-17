#pragma once

#include "AudioEngine.h"
#include "SystemHealth.h"
#include "TempoEngine.h"

namespace stagerack
{

/** Writes the dropouts of the live engine to a text file (dropouts.log in the data folder),
    with what the Mac was doing at that moment, so a glitch heard on stage can be traced back
    later. Message thread: poll() from a timer. */
class DropoutLog
{
public:
    DropoutLog (AudioEngine& e, juce::File logFile) : engine (e), file (std::move (logFile)) {}

    juce::File getFile() const { return file; }

    /** Drains the events of the audio thread. `rackName` gives the name of a rack id. */
    void poll (const std::function<juce::String (int)>& rackName, const std::function<juce::String()>& context)
    {
        juce::String text;
        const auto nowSec = TempoEngine::nowSec();
        const auto now = juce::Time::getCurrentTime();

        engine.dropouts.drain ([&] (const DropoutMonitor::Event& e)
        {
            // host time (mach seconds) -> wall clock
            const auto ago = e.hostSec > 0.0 ? juce::jmax (0.0, nowSec - e.hostSec) : 0.0;
            const auto when = now - juce::RelativeTime::seconds (ago);

            juce::String line = when.formatted ("%Y-%m-%d %H:%M:%S") + "." + juce::String (when.getMilliseconds()).paddedLeft ('0', 3) + "  ";

            if (e.silent)
                line << "SILENT block (racks being changed)";
            else
            {
                if (e.skipped > 0)
                    line << "SKIPPED " << e.skipped << " callback" << (e.skipped > 1 ? "s" : "") << " (CoreAudio called late)  ";
                if (e.ms > e.budgetMs)
                    line << "LATE " << juce::String (e.ms, 3) << " ms of " << juce::String (e.budgetMs, 3) << " ms  ";
                if (e.heaviestRack >= 0)
                    line << "heaviest rack '" << rackName (e.heaviestRack) << "' " << juce::String (e.heaviestMs, 3) << " ms";
            }

            if (const auto sys = systemhealth::problems(); sys.isNotEmpty())
                line << "  [" << sys << "]";

            text << line << "\n";
        });

        if (text.isEmpty())
            return;

        if (! headerWritten)
        {
            text = "---- " + now.formatted ("%Y-%m-%d %H:%M:%S") + "  StageRack " + juce::String (STAGERACK_VERSION) + "  "
                   + juce::SystemStats::getCpuModel() + "  " + context() + "\n" + text;
            headerWritten = true;
        }

        if (file.getSize() > 2 * 1024 * 1024)
        {
            file.copyFileTo (file.getSiblingFile (file.getFileNameWithoutExtension() + ".old.log"));
            file.replaceWithText ({}, false, false, "\n");
        }

        file.appendText (text, false, false, "\n");
    }

    /** A marker line, written only if the log already has events since the last header
        (so a clean run leaves no file). */
    void note (const juce::String& what)
    {
        if (headerWritten)
            file.appendText (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S") + "  -- " + what + "\n", false, false, "\n");
    }

    /** A new device / session: the next event writes a fresh header line. */
    void newContext() { headerWritten = false; }

private:
    AudioEngine& engine;
    juce::File file;
    bool headerWritten = false;
};

} // namespace stagerack
