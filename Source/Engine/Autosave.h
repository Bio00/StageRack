#pragma once

#include "Session.h"

namespace stagerack
{

/**
    Crash insurance for the open session.

    While the app runs, a marker file says "StageRack is open"; a clean quit removes it, so if it
    is still there at the next start the previous run crashed (or was killed).

    Every few seconds the document is serialized and, only if it differs from the last saved,
    loaded or autosaved state, written to Autosave.stagerack in the app's data folder. Nothing is
    written while a session's plugins are still being created (their state would be missing) or
    during a live re-spread. Lives on the message thread; never touches the audio thread.
*/
class Autosave final : private juce::Timer
{
public:
    explicit Autosave (Session&);
    ~Autosave() override;

    static juce::File getAutosaveFile();
    static juce::File getMarkerFile();
    /** Tests only: keep the files away from the real data folder. */
    static void setDirectoryForTests (const juce::File&);

    /** Call before start(): true if the previous run did not quit cleanly. */
    static bool previousRunCrashed();

    /** The autosaved document left by a crashed run, or an invalid tree. */
    struct Recovered
    {
        juce::ValueTree root;
        juce::File sourceFile;   // the .stagerack it came from (may be none: untitled)
        juce::Time savedAt;
    };
    static Recovered readRecovered();

    /** Writes the marker and begins watching the session. */
    void start();

    /** Clean quit: stops and deletes the marker and the autosave. */
    void cleanExit();

    /** Serializes now and writes if changed (also used by the tests). Returns true if written. */
    bool saveIfChanged();

    /** One timer step (1 s); public for the tests. */
    void tick();

    static constexpr int intervalSeconds = 15;

private:
    void timerCallback() override       { tick(); }
    juce::String serialize() const;

    Session& session;
    juce::String baseline;
    bool haveBaseline = false;
    int lastGeneration = -1;
    int ticks = 0;
    bool running = false;
};

} // namespace stagerack
