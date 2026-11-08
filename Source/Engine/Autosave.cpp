#include "Autosave.h"
#include <unistd.h>

namespace stagerack
{

Autosave::Autosave (Session& s) : session (s) {}

Autosave::~Autosave()
{
    stopTimer();
}

static juce::File& testDirectory()
{
    static juce::File dir;
    return dir;
}

void Autosave::setDirectoryForTests (const juce::File& d)
{
    testDirectory() = d;
}

static juce::File autosaveDirectory()
{
    return testDirectory() != juce::File() ? testDirectory() : PluginManager::getDataDirectory();
}

juce::File Autosave::getAutosaveFile()
{
    return autosaveDirectory().getChildFile ("Autosave.stagerack");
}

juce::File Autosave::getMarkerFile()
{
    return autosaveDirectory().getChildFile ("running.lock");
}

bool Autosave::previousRunCrashed()
{
    return getMarkerFile().existsAsFile();
}

Autosave::Recovered Autosave::readRecovered()
{
    Recovered r;
    const auto f = getAutosaveFile();

    if (auto xml = juce::parseXML (f))
    {
        auto root = juce::ValueTree::fromXml (*xml);

        if (Session::isSessionTree (root))
        {
            r.sourceFile = juce::File (root["autosaveSource"].toString());
            r.savedAt = f.getLastModificationTime();
            root.removeProperty ("autosaveSource", nullptr);
            r.root = root;
        }
    }

    return r;
}

void Autosave::start()
{
    getMarkerFile().replaceWithText (juce::String ((int) getpid()));   // pid, for a human reading it
    running = true;
    lastGeneration = -1;
    startTimer (1000);
}

void Autosave::cleanExit()
{
    stopTimer();

    if (running)
    {
        getAutosaveFile().deleteFile();
        getMarkerFile().deleteFile();
    }

    running = false;
}

juce::String Autosave::serialize() const
{
    return session.toTree().toXmlString();
}

bool Autosave::saveIfChanged()
{
    auto root = session.toTree();
    auto text = root.toXmlString();

    if (haveBaseline && text == baseline)
        return false;

    root.setProperty ("autosaveSource", session.getFile().getFullPathName(), nullptr);

    auto xml = root.createXml();

    if (xml == nullptr || ! xml->writeTo (getAutosaveFile()))   // writeTo goes through a temporary file
        return false;

    baseline = text;
    haveBaseline = true;
    return true;
}

void Autosave::tick()
{
    // a half-loaded session would be autosaved without its plugins
    if (session.getPendingLoads() > 0 || session.isRespreading())
        return;

    if (session.getDocumentGeneration() != lastGeneration)
    {
        // new / opened / saved: what's on disk is the reference, nothing to protect yet
        lastGeneration = session.getDocumentGeneration();
        haveBaseline = false;
        baseline = {};
        ticks = 0;

        if (! session.isDirty())
        {
            baseline = serialize();
            haveBaseline = true;
            getAutosaveFile().deleteFile();   // the saved file is now newer than any autosave
            return;
        }
        // changed already (e.g. a recovered session): autosave at once
        ticks = intervalSeconds;
    }

    if (++ticks < intervalSeconds)
        return;

    ticks = 0;
    saveIfChanged();
}

} // namespace stagerack
