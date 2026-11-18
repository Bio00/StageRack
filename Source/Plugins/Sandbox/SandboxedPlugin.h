#pragma once

#include <JuceHeader.h>
#include <atomic>

namespace stagerack::sandbox
{

class Link;

/*  EXPERIMENTAL: a plugin hosted in a child process (see SandboxShared.h). For the rest of
    StageRack it is an ordinary AudioPluginInstance: same description (sessions are saved exactly
    as with the plugin in process), parameters mirrored, state through the pipe, processBlock in
    the same callback through shared memory (zero added latency).
    If the child crashes or hangs, the slot passes its input through (dry) and the child is
    restarted with the last known state (refreshed every 5 s and at every getState). */
class SandboxedPlugin final : public juce::AudioPluginInstance,
                              private juce::Timer
{
public:
    using Callback = std::function<void (std::unique_ptr<juce::AudioPluginInstance>, const juce::String& error)>;

    /** Launches the child and loads the plugin there (on a background thread); the callback
        comes on the message thread. */
    static void createAsync (const juce::PluginDescription&, double sampleRate, int blockSize, Callback);

    /** Same, blocking (tests). */
    static std::unique_ptr<SandboxedPlugin> createBlocking (const juce::PluginDescription&, double sampleRate, int blockSize, juce::String& error);

    ~SandboxedPlugin() override;

    // diagnostics / tests
    int getRestartCount() const noexcept             { return restarts.load(); }
    juce::uint32 getDryBlocks() const noexcept       { return dryBlocks.load(); }
    juce::uint32 getLateBlocks() const noexcept      { return lateBlocks.load(); }
    bool isChildAlive() const noexcept               { return alive.load(); }
    float getChildProcessMs() const noexcept;
    double getRoundTripMs() const noexcept           { return lastRoundTripMs.load(); }
    void crashChildForTests();
    void setWaitFractionForTests (double f) noexcept { waitFraction = f; }   // live: 0.8 of the block
    static inline std::atomic<double> defaultSpinMicros { 20.0 };             // tests: --sandbox-spin <us>
    void hangChildForTests (int ms);
    void showChildEditor();
    void hideChildEditor();

    // AudioPluginInstance
    void fillInPluginDescription (juce::PluginDescription& d) const override   { d = description; }
    const juce::String getName() const override                             { return description.name; }
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override;
    void reset() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    using juce::AudioPluginInstance::processBlockBypassed;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    double getTailLengthSeconds() const override    { return tail; }
    bool acceptsMidi() const override               { return false; }
    bool producesMidi() const override              { return false; }
    bool hasEditor() const override                 { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override                   { return 1; }
    int getCurrentProgram() override                { return 0; }
    void setCurrentProgram (int) override           {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    void pushParameter (int index, float value);
    juce::String parameterText (int index) const;

    static bool isSandboxed (const juce::AudioProcessor* p) noexcept { return dynamic_cast<const SandboxedPlugin*> (p) != nullptr; }

private:
    SandboxedPlugin (const juce::PluginDescription&, const juce::ValueTree& loadReply, std::shared_ptr<Link>);
    static BusesProperties busesFor (const juce::ValueTree& loadReply);

    void timerCallback() override;
    void childLost();
    void restart();
    void passThrough (juce::AudioBuffer<float>&) noexcept;
    void process (juce::AudioBuffer<float>&, bool bypassed) noexcept;

    juce::PluginDescription description;
    int numIns = 2, numOuts = 2;
    double tail = 0.0;

    std::shared_ptr<Link> link;                       // current child (shared: background state requests keep it alive)
    std::vector<std::shared_ptr<Link>> deadLinks;     // crashed ones: their shared memory may still be read by a late audio block
    std::atomic<Link*> active { nullptr };
    std::atomic<bool> alive { false }, restarting { false }, destroying { false };
    std::atomic<int> restarts { 0 };
    std::atomic<juce::uint32> dryBlocks { 0 }, lateBlocks { 0 };
    std::atomic<double> lastRoundTripMs { 0.0 };
    std::atomic<double> waitFraction { 0.8 };
    std::atomic<double> spinMicros { 20.0 };   // then sleep (os_sync, macOS 14.4+)
    std::atomic<std::uint32_t> inFlight { 0 };       // audio thread: request not answered in time
    juce::SpinLock paramLock;
    juce::MemoryBlock lastState;
    juce::CriticalSection stateLock;
    double preparedRate = 0.0;
    int preparedBlock = 0, ticks = 0;
    std::shared_ptr<bool> aliveToken = std::make_shared<bool> (true);

    class Param;
    juce::Array<Param*> mirrored;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SandboxedPlugin)
};

} // namespace stagerack::sandbox
