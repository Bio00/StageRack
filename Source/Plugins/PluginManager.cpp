#include "PluginManager.h"
#include "CLAPPluginFormat.h"
#include "../Engine/Rack.h"
#include <sys/sysctl.h>

namespace stagerack
{

/** True when this (universal) app runs under Rosetta, e.g. "Open using Rosetta"
    to host Intel-only plugins. */
static bool isRunningUnderRosetta()
{
    int translated = 0;
    size_t size = sizeof (translated);
    return sysctlbyname ("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0 && translated == 1;
}

//==============================================================================
/** Scans one plugin file/identifier in a child process. */
class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override
    {
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        juce::TemporaryFile out (".xml");

        juce::ChildProcess child;
        juce::StringArray args { exe.getFullPathName(), PluginManager::scanArgument,
                                 format.getName(), fileOrIdentifier,
                                 out.getFile().getFullPathName() };

        // A universal child would start as arm64 and could not load the Intel-only
        // plugins this Rosetta instance is able to host: keep it on the same architecture.
        if (isRunningUnderRosetta())
        {
            args.insert (0, "-x86_64");
            args.insert (0, "/usr/bin/arch");
        }

        if (! child.start (args, 0))
            return true; // could not launch: don't blacklist the plugin

        constexpr int timeoutMs = 60000;
        const auto startMs = juce::Time::getMillisecondCounter();

        while (child.isRunning())
        {
            if (shouldExit())
            {
                child.kill();
                return true;
            }

            if (juce::Time::getMillisecondCounter() - startMs > (juce::uint32) timeoutMs)
            {
                child.kill();
                return false; // hung -> blacklist
            }

            juce::Thread::sleep (20);
        }

        // A complete result file counts even if the child crashed afterwards: many plugins
        // crash in their static destructors when unloaded, which is harmless for us.
        if (auto xml = juce::parseXML (out.getFile()); xml != nullptr && xml->hasTagName ("PLUGINS"))
        {
            for (auto* e : xml->getChildIterator())
            {
                auto d = std::make_unique<juce::PluginDescription>();

                if (d->loadFromXml (*e))
                    result.add (d.release());
            }

            return true;
        }

        return false; // crashed or hung while scanning -> blacklist
    }
};

//==============================================================================
PluginManager::PluginManager()
{
    addAllFormats (formatManager);

    juce::PropertiesFile::Options opts;
    opts.applicationName = "StageRack";
    opts.filenameSuffix = ".settings";
    opts.folderName = "StageRack";
    opts.osxLibrarySubFolder = "Application Support";
    settings = std::make_unique<juce::PropertiesFile> (opts);

    if (auto xml = juce::parseXML (getDataDirectory().getChildFile ("plugins.xml")))
        knownList.recreateFromXml (*xml);

    knownList.setCustomScanner (std::make_unique<OutOfProcessScanner>());
    knownList.addChangeListener (this);

    if (auto xml = juce::parseXML (getDataDirectory().getChildFile ("latency.xml")))
        for (auto* e : xml->getChildIterator())
            latencies[e->getStringAttribute ("id")] = (float) e->getDoubleAttribute ("ms", latencyUnknown);
}

PluginManager::~PluginManager()
{
    stopLatencyProbe();
    knownList.removeChangeListener (this);
    saveList();
    saveLatencies();
}

juce::File PluginManager::getDataDirectory()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Application Support/StageRack");
    dir.createDirectory();
    return dir;
}

juce::File PluginManager::getDeadMansPedalFile() const
{
    return getDataDirectory().getChildFile ("scan-in-progress.txt");
}

void PluginManager::addAllFormats (juce::AudioPluginFormatManager& fm)
{
    juce::addDefaultFormatsToManager (fm);
    fm.addFormat (std::make_unique<CLAPPluginFormat>());
}

void PluginManager::changeListenerCallback (juce::ChangeBroadcaster*)
{
    saveList();
}

void PluginManager::saveList()
{
    if (auto xml = knownList.createXml())
        xml->writeTo (getDataDirectory().getChildFile ("plugins.xml"));
}

void PluginManager::createPlugin (const juce::PluginDescription& desc, double sampleRate, int blockSize, Callback cb)
{
    formatManager.createPluginInstanceAsync (desc, sampleRate, blockSize,
        [cb = std::move (cb)] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& err) mutable
        {
            if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            {
                cb (std::move (p), err);
            }
            else
            {
                auto shared = std::make_shared<std::unique_ptr<juce::AudioPluginInstance>> (std::move (p));
                juce::MessageManager::callAsync ([cb, shared, err] { cb (std::move (*shared), err); });
            }
        });
}

std::optional<juce::PluginDescription> PluginManager::findKnown (const juce::PluginDescription& saved) const
{
    const auto types = knownList.getTypes();

    for (auto& t : types)
        if (t.isDuplicateOf (saved))
            return t;

    for (auto& t : types)
        if (t.pluginFormatName == saved.pluginFormatName && t.name == saved.name && t.manufacturerName == saved.manufacturerName)
            return t;

    if (saved.fileOrIdentifier.isNotEmpty())
        return saved;

    return std::nullopt;
}

//==============================================================================
/** Background thread: runs "StageRack --probe-latency <desc.xml> <out.xml>" children, a few at a time. */
class PluginManager::LatencyProbe final : public juce::Thread
{
public:
    LatencyProbe (PluginManager& o, juce::Array<juce::PluginDescription> todo)
        : juce::Thread ("Latency probe"), owner (o), pending (std::move (todo)), total (pending.size()) {}

    ~LatencyProbe() override { stopThread (4000); }

    std::atomic<int> done { 0 };
    const int total;

    void run() override
    {
        struct Job
        {
            juce::PluginDescription desc;
            std::unique_ptr<juce::ChildProcess> child;
            std::unique_ptr<juce::TemporaryFile> in, out;
            juce::uint32 start = 0;
        };

        std::vector<Job> running;
        constexpr int parallel = 3;
        constexpr juce::uint32 timeoutMs = 30000;
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        int next = 0;

        while (! threadShouldExit() && (next < pending.size() || ! running.empty()))
        {
            while ((int) running.size() < parallel && next < pending.size())
            {
                Job job;
                job.desc = pending[next++];
                job.in = std::make_unique<juce::TemporaryFile> (".xml");
                job.out = std::make_unique<juce::TemporaryFile> (".xml");
                job.desc.createXml()->writeTo (job.in->getFile());
                job.child = std::make_unique<juce::ChildProcess>();

                juce::StringArray args { exe.getFullPathName(), latencyArgument,
                                         job.in->getFile().getFullPathName(), job.out->getFile().getFullPathName() };

                if (isRunningUnderRosetta())
                {
                    args.insert (0, "-x86_64");
                    args.insert (0, "/usr/bin/arch");
                }

                if (! job.child->start (args, 0))
                {
                    owner.setLatency (job.desc, latencyFailed);
                    ++done;
                    continue;
                }

                job.start = juce::Time::getMillisecondCounter();
                running.push_back (std::move (job));
            }

            for (auto it = running.begin(); it != running.end();)
            {
                const bool timedOut = juce::Time::getMillisecondCounter() - it->start > timeoutMs;

                if (it->child->isRunning() && ! timedOut)
                {
                    ++it;
                    continue;
                }

                if (timedOut)
                    it->child->kill();

                float ms = latencyFailed;

                if (auto xml = juce::parseXML (it->out->getFile()); xml != nullptr && xml->hasTagName ("LATENCY"))
                    ms = (float) (1000.0 * xml->getIntAttribute ("samples") / juce::jmax (1.0, xml->getDoubleAttribute ("rate", 48000.0)));

                owner.setLatency (it->desc, ms);
                ++done;
                it = running.erase (it);
            }

            wait (30);
        }

        for (auto& j : running)
            j.child->kill();

        owner.saveLatencies();
    }

private:
    PluginManager& owner;
    juce::Array<juce::PluginDescription> pending;
};

float PluginManager::getLatencyMs (const juce::PluginDescription& d) const
{
    const juce::ScopedLock sl (latencyLock);
    const auto it = latencies.find (d.createIdentifierString());
    return it != latencies.end() ? it->second : latencyUnknown;
}

void PluginManager::setLatency (const juce::PluginDescription& d, float ms)
{
    const juce::ScopedLock sl (latencyLock);
    auto& v = latencies[d.createIdentifierString()];

    if (v != ms)
    {
        v = ms;
        latenciesDirty = true;
    }
}

void PluginManager::recordLatency (const juce::PluginDescription& d, int samples, double sampleRate)
{
    if (sampleRate <= 0.0 || samples < 0)
        return;

    const auto ms = (float) (1000.0 * samples / sampleRate);

    if (std::abs (getLatencyMs (d) - ms) > 0.001f)
    {
        setLatency (d, ms);
        saveLatencies();
    }
}

void PluginManager::saveLatencies()
{
    juce::XmlElement root ("LATENCIES");

    {
        const juce::ScopedLock sl (latencyLock);

        if (! latenciesDirty)
            return;

        for (auto& [id, ms] : latencies)
        {
            auto* e = root.createNewChildElement ("P");
            e->setAttribute ("id", id);
            e->setAttribute ("ms", ms);
        }

        latenciesDirty = false;
    }

    root.writeTo (getDataDirectory().getChildFile ("latency.xml"));
}

int PluginManager::getNumUnknownLatencies() const
{
    int n = 0;

    for (auto& d : knownList.getTypes())
        if (getLatencyMs (d) == latencyUnknown && ! d.isInstrument)   // instruments are never probed
            ++n;

    return n;
}

void PluginManager::startLatencyProbe()
{
    if (isProbingLatency())
        return;

    juce::Array<juce::PluginDescription> todo;

    for (auto& d : knownList.getTypes())
        if (getLatencyMs (d) == latencyUnknown && ! d.isInstrument)
            todo.add (d);

    if (todo.isEmpty())
        return;

    latencyProbe = std::make_unique<LatencyProbe> (*this, std::move (todo));
    latencyProbe->startThread (juce::Thread::Priority::low);
}

void PluginManager::stopLatencyProbe()
{
    latencyProbe = nullptr;
}

bool PluginManager::isProbingLatency() const
{
    return latencyProbe != nullptr && latencyProbe->isThreadRunning();
}

std::pair<int, int> PluginManager::getLatencyProbeProgress() const
{
    if (latencyProbe == nullptr)
        return { 0, 0 };

    return { latencyProbe->done.load(), latencyProbe->total };
}

int PluginManager::runLatencyProbeChild (const juce::StringArray& args)
{
    const auto idx = args.indexOf (latencyArgument);

    if (idx < 0 || args.size() < idx + 3)
        return 2;

    juce::PluginDescription desc;

    if (auto xml = juce::parseXML (juce::File (args[idx + 1])); xml == nullptr || ! desc.loadFromXml (*xml))
        return 3;

    juce::AudioPluginFormatManager fm;
    addAllFormats (fm);

    constexpr double rate = 48000.0;
    constexpr int block = 64;
    juce::String error;
    auto p = fm.createPluginInstance (desc, rate, block, error);

    if (p == nullptr)
        return 4;

    Rack::configurePlugin (*p, rate, block);

    // a few blocks of silence: some plugins only settle their reported latency once running
    juce::AudioBuffer<float> buffer (juce::jmax (2, p->getTotalNumInputChannels(), p->getTotalNumOutputChannels()), block);
    juce::MidiBuffer midi;

    for (int i = 0; i < 8; ++i)
    {
        buffer.clear();
        p->processBlock (buffer, midi);
    }

    juce::XmlElement root ("LATENCY");
    root.setAttribute ("samples", p->getLatencySamples());
    root.setAttribute ("rate", rate);
    return root.writeTo (juce::File (args[idx + 2])) ? 0 : 5;
}

//==============================================================================
int PluginManager::runScannerChild (const juce::StringArray& args)
{
    const auto idx = args.indexOf (scanArgument);

    if (idx < 0 || args.size() < idx + 4)
        return 2;

    const auto formatName = args[idx + 1];
    const auto identifier = args[idx + 2];
    const juce::File outFile (args[idx + 3]);

    juce::AudioPluginFormatManager fm;
    addAllFormats (fm);

    for (auto* format : fm.getFormats())
    {
        if (format->getName() != formatName)
            continue;

        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile (found, identifier);

        juce::XmlElement root ("PLUGINS");
        for (auto* d : found)
            root.addChildElement (d->createXml().release());

        return root.writeTo (outFile) ? 0 : 3;
    }

    return 4;
}

} // namespace stagerack
