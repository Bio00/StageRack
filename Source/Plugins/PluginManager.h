#pragma once

#include <JuceHeader.h>
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

    juce::AudioPluginFormatManager& getFormatManager() noexcept    { return formatManager; }
    juce::KnownPluginList& getKnownList() noexcept                  { return knownList; }
    juce::PropertiesFile& getSettings() noexcept                    { return *settings; }
    juce::File getDeadMansPedalFile() const;

    using Callback = std::function<void (std::unique_ptr<juce::AudioPluginInstance>, const juce::String& error)>;

    /** Creates a plugin asynchronously; the callback always runs on the message thread. */
    void createPlugin (const juce::PluginDescription&, double sampleRate, int blockSize, Callback);

    /** Finds a known plugin from a saved description (matching by uid/identifier). */
    std::optional<juce::PluginDescription> findKnown (const juce::PluginDescription&) const;

    /** Entry point of the scanner child process. Returns the process exit code. */
    static int runScannerChild (const juce::StringArray& args);
    static constexpr const char* scanArgument = "--scan-plugin";

    static juce::File getDataDirectory();

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
