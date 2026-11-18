#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <map>

namespace stagerack
{

/**
    Plugin formats, the list of known plugins and plugin creation.

    Scanning runs each plugin in a separate child process ("StageRack --scan-plugin ..."),
    several in parallel; a plugin that crashes or hangs while scanning is blacklisted
    instead of taking the host down.
*/
class PluginManager final : private juce::ChangeListener
{
public:
    PluginManager();
    ~PluginManager() override;

    static void addAllFormats (juce::AudioPluginFormatManager&);

    /** Settings a plugin needs when the user inserts it fresh (not when it comes back from a
        session, a copy or a scene, which keep their own state). Returns what it changed.
        EverClean (Everworks) loads with "Active" off, i.e. doing nothing: switched on here,
        so it works right away and its hop is seen (and spread) at insertion. */
    static juce::String applyInsertDefaults (juce::AudioPluginInstance&);

    juce::AudioPluginFormatManager& getFormatManager() noexcept    { return formatManager; }
    juce::KnownPluginList& getKnownList() noexcept                  { return knownList; }
    juce::PropertiesFile& getSettings() noexcept                    { return *settings; }
    juce::File getDeadMansPedalFile() const;

    using Callback = std::function<void (std::unique_ptr<juce::AudioPluginInstance>, const juce::String& error)>;

    /** Creates a plugin asynchronously; the callback always runs on the message thread. */
    void createPlugin (const juce::PluginDescription&, double sampleRate, int blockSize, Callback);

    /** EXPERIMENTAL (setting "isolatePlugins"): plugins created from now on run in a child process
        (sandbox::SandboxedPlugin). */
    static inline std::atomic<bool> isolatePlugins { false };

    /** Finds a known plugin from a saved description (matching by uid/identifier). */
    std::optional<juce::PluginDescription> findKnown (const juce::PluginDescription&) const;

    /** Entry point of the scanner child process. Returns the process exit code. */
    static int runScannerChild (const juce::StringArray& args);
    static constexpr const char* scanArgument = "--scan-plugin";

    static juce::File getDataDirectory();

    /** Stops the background probe and writes plugin list, latencies and settings now. */
    void saveAll();

    //==============================================================================
    // Plugin latency, shown in the browser. Learned whenever a plugin is loaded, or measured
    // on request in child processes (same isolation as the scan).
    static constexpr float latencyUnknown = -1.0f, latencyFailed = -2.0f;

    /** Latency in ms, or latencyUnknown / latencyFailed. Any thread. */
    float getLatencyMs (const juce::PluginDescription&) const;
    void recordLatency (const juce::PluginDescription&, int samples, double sampleRate);

    /** Measures every plugin whose latency is unknown, in the background. */
    void startLatencyProbe();
    void stopLatencyProbe();
    bool isProbingLatency() const;
    int getNumUnknownLatencies() const;
    std::pair<int, int> getLatencyProbeProgress() const;   // done, total

    static int runLatencyProbeChild (const juce::StringArray& args);
    static constexpr const char* latencyArgument = "--probe-latency";

private:
    class LatencyProbe;
    void saveLatencies();
    void setLatency (const juce::PluginDescription&, float ms);

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void saveList();

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownList;
    std::unique_ptr<juce::PropertiesFile> settings;

    mutable juce::CriticalSection latencyLock;
    std::map<juce::String, float> latencies;   // createIdentifierString() -> ms
    bool latenciesDirty = false;
    std::unique_ptr<LatencyProbe> latencyProbe;
};

} // namespace stagerack
