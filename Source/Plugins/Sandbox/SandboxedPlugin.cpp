#include "SandboxedPlugin.h"
#include "SandboxShared.h"
#include "../../Engine/RtLock.h"
#include "../../Engine/RtGuard.h"
#include <map>

namespace stagerack::sandbox
{

//==============================================================================
/** One child process: pipe (control) + shared memory (audio). */
class Link final : public juce::ChildProcessCoordinator
{
public:
    std::unique_ptr<Channel> channel;
    std::function<void()> onLost;                                   // any thread
    std::function<void (const juce::ValueTree&)> onEvent;           // any thread
    std::atomic<bool> lost { false };

    ~Link() override
    {
        if (channel != nullptr)
        {
            channel->get().quit = 1;
            channel->signal();
        }
        onLost = nullptr;
        killWorkerProcess();
    }

    bool start (juce::String& error)
    {
        static std::atomic<int> counter { 0 };
        const auto name = "srk" + juce::String (getpid()) + "_" + juce::String (counter++);
        channel = Channel::create (name);
        if (channel == nullptr) { error = "cannot create shared memory"; return false; }

        if (! launchWorkerProcess (juce::File::getSpecialLocation (juce::File::currentExecutableFile), commandLineId, 10000, 0))
        {
            error = "cannot start the sandbox process";
            return false;
        }
        return true;
    }

    /** Request + reply (blocks the calling thread up to timeoutMs; invalid tree on failure). */
    juce::ValueTree call (juce::ValueTree msg, int timeoutMs)
    {
        if (lost.load()) return {};

        auto pending = std::make_shared<Pending>();
        int id;
        {
            const juce::ScopedLock sl (lock);
            id = ++nextId;
            waiting[id] = pending;
        }

        msg.setProperty ("id", id, nullptr);
        if (! sendMessageToWorker (pack (msg)))
        {
            const juce::ScopedLock sl (lock);
            waiting.erase (id);
            return {};
        }

        pending->done.wait (timeoutMs);
        const juce::ScopedLock sl (lock);
        waiting.erase (id);
        return pending->reply;
    }

    void post (juce::ValueTree msg)
    {
        if (! lost.load())
            sendMessageToWorker (pack (msg));
    }

    void handleMessageFromWorker (const juce::MemoryBlock& mb) override
    {
        auto t = unpack (mb);
        if (t.hasType ("Reply"))
        {
            const juce::ScopedLock sl (lock);
            if (auto it = waiting.find ((int) t["id"]); it != waiting.end())
            {
                it->second->reply = t;
                it->second->done.signal();
            }
            return;
        }
        if (onEvent) onEvent (t);
    }

    void handleConnectionLost() override
    {
        lost = true;
        {
            const juce::ScopedLock sl (lock);
            for (auto& [id, p] : waiting)
                p->done.signal();
        }
        if (onLost) onLost();
    }

private:
    struct Pending { juce::WaitableEvent done; juce::ValueTree reply; };
    juce::CriticalSection lock;
    std::map<int, std::shared_ptr<Pending>> waiting;
    int nextId = 0;
};

static juce::ValueTree loadMessage (const juce::PluginDescription& desc, const Link& link, double sr, int bs, const juce::MemoryBlock& state)
{
    juce::ValueTree m ("load");
    m.setProperty ("channel", link.channel->getName(), nullptr);
    m.setProperty ("desc", desc.createXml()->toString (juce::XmlElement::TextFormat().singleLine()), nullptr);
    m.setProperty ("sr", sr, nullptr);
    m.setProperty ("bs", bs, nullptr);
    if (state.getSize() > 0)
        m.setProperty ("state", state, nullptr);
    return m;
}

static constexpr int loadTimeoutMs = 60000;   // some plugins (Waves shells, licences) take long

//==============================================================================
class SandboxedPlugin::Param final : public juce::AudioPluginInstance::HostedParameter
{
public:
    Param (SandboxedPlugin& o, int i, const juce::ValueTree& t)
        : owner (o), index (i), name (t["name"].toString()), label (t["label"].toString()), pid (t["pid"].toString()),
          def ((float) t["def"]), steps ((int) t["steps"]), discrete ((bool) t["discrete"]), boolean ((bool) t["bool"]),
          automatable ((bool) t["auto"]), meta ((bool) t["meta"]),
          strings (juce::StringArray::fromLines (t["strings"].toString()))
    {
        value = (float) t["value"];
        if (pid.isEmpty()) pid = juce::String (i);
    }

    float getValue() const override                 { return value.load(); }
    void setValue (float v) override
    {
        value = v;
        setAt = juce::Time::getMillisecondCounter();
        owner.pushParameter (index, v);
    }
    float getDefaultValue() const override          { return def; }
    juce::String getName (int max) const override   { return name.substring (0, max); }
    juce::String getLabel() const override          { return label; }
    int getNumSteps() const override                { return steps; }
    bool isDiscrete() const override                { return discrete; }
    bool isBoolean() const override                 { return boolean; }
    bool isAutomatable() const override             { return automatable; }
    bool isMetaParameter() const override           { return meta; }
    juce::String getParameterID() const override    { return pid; }
    juce::StringArray getAllValueStrings() const override { return discrete && ! strings.isEmpty() ? strings : juce::AudioProcessorParameter::getAllValueStrings(); }

    juce::String getText (float v, int max) const override
    {
        if (std::abs (v - value.load()) < 1.0e-5f)
            if (auto t = owner.parameterText (index); t.isNotEmpty())
                return t.substring (0, max);
        if (discrete && strings.size() > 1)
            return strings[juce::jlimit (0, strings.size() - 1, juce::roundToInt (v * (float) (strings.size() - 1)))];
        return juce::String (v, 3);
    }

    float getValueForText (const juce::String& text) const override
    {
        if (discrete && strings.contains (text))
            return (float) strings.indexOf (text) / (float) juce::jmax (1, strings.size() - 1);
        return juce::jlimit (0.0f, 1.0f, text.getFloatValue());
    }

    /** Message thread: the child's value, unless the host set one just now. */
    void sync (float childValue)
    {
        if (juce::Time::getMillisecondCounter() - setAt.load() < 400)
            return;
        if (std::abs (childValue - value.load()) > 1.0e-6f)
        {
            value = childValue;
            sendValueChangedMessageToListeners (childValue);
        }
    }

    const int index;

private:
    SandboxedPlugin& owner;
    const juce::String name, label;
    juce::String pid;
    const float def;
    const int steps;
    const bool discrete, boolean, automatable, meta;
    const juce::StringArray strings;
    std::atomic<float> value { 0.0f };
    std::atomic<juce::uint32> setAt { 0 };
};

//==============================================================================
/** In StageRack's own window: the plugin's real editor lives in the child's window. */
class SandboxEditor final : public juce::AudioProcessorEditor
{
public:
    explicit SandboxEditor (SandboxedPlugin& p) : juce::AudioProcessorEditor (p), plugin (p)
    {
        show.setButtonText ("Show plugin window");
        show.onClick = [this] { plugin.showChildEditor(); };
        addAndMakeVisible (show);
        setSize (360, 110);
        plugin.showChildEditor();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1e2126));
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (plugin.getName() + " runs isolated", 12, 10, getWidth() - 24, 20, juce::Justification::centredLeft);
        g.setFont (juce::FontOptions (12.0f));
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.drawText ("Experimental: its window is opened by its own process.", 12, 32, getWidth() - 24, 18, juce::Justification::centredLeft);
    }

    ~SandboxEditor() override { plugin.hideChildEditor(); }

    void resized() override { show.setBounds (12, 64, 180, 30); }

private:
    SandboxedPlugin& plugin;
    juce::TextButton show;
};

//==============================================================================
SandboxedPlugin::BusesProperties SandboxedPlugin::busesFor (const juce::ValueTree& r)
{
    auto set = [] (int n) { return n == 1 ? juce::AudioChannelSet::mono() : n == 2 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::discreteChannels (n); };
    BusesProperties b;
    if ((int) r["ins"] > 0)  b = b.withInput ("Input", set ((int) r["ins"]), true);
    if ((int) r["outs"] > 0) b = b.withOutput ("Output", set ((int) r["outs"]), true);
    return b;
}

SandboxedPlugin::SandboxedPlugin (const juce::PluginDescription& d, const juce::ValueTree& r, std::shared_ptr<Link> l)
    : juce::AudioPluginInstance (busesFor (r)), description (d), link (std::move (l))
{
    numIns = juce::jlimit (0, Shared::maxChannels, (int) r["ins"]);
    numOuts = juce::jlimit (0, Shared::maxChannels, (int) r["outs"]);
    tail = r["tail"];
    setLatencySamples (r["latency"]);

    for (int i = 0; i < juce::jmin (r.getNumChildren(), Shared::maxParams); ++i)
    {
        auto p = std::make_unique<Param> (*this, i, r.getChild (i));
        mirrored.add (p.get());
        addHostedParameter (std::move (p));
    }

    std::weak_ptr<bool> weak = aliveToken;
    link->onLost = [this, weak] { juce::MessageManager::callAsync ([this, weak] { if (weak.lock() != nullptr) childLost(); }); };
    link->onEvent = [this, weak] (const juce::ValueTree& ev)
    {
        if (ev.hasType ("latency"))
            juce::MessageManager::callAsync ([this, weak, l = (int) ev["latency"]] { if (weak.lock() != nullptr) setLatencySamples (l); });
    };

    spinMicros = defaultSpinMicros.load();
    active = link.get();
    alive = true;
    startTimerHz (30);
}

SandboxedPlugin::~SandboxedPlugin()
{
    destroying = true;
    stopTimer();
    aliveToken.reset();
    alive = false;
    active = nullptr;
    link.reset();
    deadLinks.clear();
}

std::unique_ptr<SandboxedPlugin> SandboxedPlugin::createBlocking (const juce::PluginDescription& desc, double sr, int bs, juce::String& error)
{
    auto link = std::make_unique<Link>();
    if (! link->start (error))
        return {};

    const auto r = link->call (loadMessage (desc, *link, sr, bs, {}), loadTimeoutMs);
    if (! r.isValid()) { error = "the sandbox process did not answer"; return {}; }
    if (r.hasProperty ("error")) { error = r["error"].toString(); return {}; }

    auto p = std::unique_ptr<SandboxedPlugin> (new SandboxedPlugin (desc, r, std::move (link)));
    p->setRateAndBufferSizeDetails (sr, bs);
    p->preparedRate = sr;
    p->preparedBlock = bs;
    return p;
}

void SandboxedPlugin::createAsync (const juce::PluginDescription& desc, double sr, int bs, Callback cb)
{
    // the child loads the plugin: seconds for some, never on the message thread
    juce::Thread::launch ([desc, sr, bs, cb = std::move (cb)]
    {
        juce::String error;
        auto link = std::make_unique<Link>();
        juce::ValueTree r;

        if (link->start (error))
        {
            r = link->call (loadMessage (desc, *link, sr, bs, {}), loadTimeoutMs);
            if (! r.isValid())             error = "the sandbox process did not answer";
            else if (r.hasProperty ("error")) error = r["error"].toString();
        }

        auto shared = std::make_shared<std::unique_ptr<Link>> (std::move (link));
        juce::MessageManager::callAsync ([desc, sr, bs, r, error, shared, cb]
        {
            if (error.isNotEmpty() || ! r.isValid())
            {
                cb (nullptr, error);
                return;
            }
            auto p = std::unique_ptr<SandboxedPlugin> (new SandboxedPlugin (desc, r, std::move (*shared)));
            p->setRateAndBufferSizeDetails (sr, bs);
            p->preparedRate = sr;
            p->preparedBlock = bs;
            cb (std::move (p), {});
        });
    });
}

//==============================================================================
bool SandboxedPlugin::isBusesLayoutSupported (const BusesLayout& layout) const
{
    // the child chose its layout (Rack::configurePlugin over there): only that one
    return layout.getMainInputChannels() == numIns && layout.getMainOutputChannels() == numOuts;
}

void SandboxedPlugin::prepareToPlay (double sr, int bs)
{
    if (sr == preparedRate && bs == preparedBlock)
        return;
    preparedRate = sr;
    preparedBlock = bs;

    if (auto* l = active.load(); l != nullptr && alive.load())
    {
        juce::ValueTree m ("prepare");
        m.setProperty ("sr", sr, nullptr);
        m.setProperty ("bs", bs, nullptr);
        if (const auto r = l->call (m, 10000); r.isValid())
            setLatencySamples (r["latency"]);
        inFlight = 0;
    }
}

void SandboxedPlugin::releaseResources()
{
    if (auto* l = active.load()) l->post (juce::ValueTree ("release"));
}

void SandboxedPlugin::reset()
{
    if (auto* l = active.load()) l->post (juce::ValueTree ("reset"));
}

void SandboxedPlugin::passThrough (juce::AudioBuffer<float>& buffer) noexcept
{
    // dry: the input stays; a mono -> stereo plugin gets its left channel copied right
    dryBlocks.fetch_add (1, std::memory_order_relaxed);
    if (numIns == 1 && buffer.getNumChannels() > 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, buffer.getNumSamples());
    for (int c = juce::jmax (numIns, 2); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
}

void SandboxedPlugin::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)          { process (buffer, false); }
void SandboxedPlugin::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)  { process (buffer, true); }

void SandboxedPlugin::process (juce::AudioBuffer<float>& buffer, bool bypassed) noexcept
{
    const rt::HostScope host;   // our code, even though the rack calls it as a plugin
    auto* l = active.load (std::memory_order_acquire);

    if (l == nullptr || ! alive.load (std::memory_order_relaxed) || l->lost.load (std::memory_order_relaxed))
    {
        passThrough (buffer);
        return;
    }

    auto& s = l->channel->get();
    const int total = buffer.getNumSamples();

    // the previous request is still being processed (the child was late): never two in flight
    if (const auto pending = inFlight.load (std::memory_order_relaxed); pending != 0)
    {
        if (s.done.load (std::memory_order_acquire) != pending)
        {
            passThrough (buffer);
            return;
        }
        inFlight = 0;
    }

    // the play head of this block, for tempo-synced plugins
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            s.bpm = pos->getBpm().orFallback (120.0);
            s.ppq = pos->getPpqPosition().orFallback (0.0);
            s.timeSec = pos->getTimeInSeconds().orFallback (0.0);
            s.playing = pos->getIsPlaying() ? 1 : 0;
            if (auto ts = pos->getTimeSignature()) { s.timeSigNum = ts->numerator; s.timeSigDen = ts->denominator; }
            s.hasPosition = 1;
        }

    const double rate = juce::jmax (1.0, getSampleRate());
    static const auto ticksPerMs = (double) juce::Time::getHighResolutionTicksPerSecond() * 1.0e-3;

    for (int start = 0; start < total; start += Shared::maxBlock)
    {
        const int n = juce::jmin (Shared::maxBlock, total - start);
        const int ins = juce::jmin (numIns, buffer.getNumChannels());

        for (int c = 0; c < ins; ++c)
            std::memcpy (s.audioIn[c], buffer.getReadPointer (c, start), sizeof (float) * (size_t) n);

        s.numSamples.store (n, std::memory_order_relaxed);
        s.numInputs.store (ins, std::memory_order_relaxed);
        s.numOutputs.store (juce::jmin (numOuts, buffer.getNumChannels()), std::memory_order_relaxed);
        s.bypassed.store (bypassed ? 1 : 0, std::memory_order_relaxed);

        const auto seq = s.request.load (std::memory_order_relaxed) + 1;
        const auto t0 = juce::Time::getHighResolutionTicks();
        s.request.store (seq, std::memory_order_release);
        l->channel->signal();

        // wait for the child within this callback: up to 80% of the block's duration
        const auto limit = (juce::int64) (waitFraction.load (std::memory_order_relaxed) * 1000.0 * n / rate * ticksPerMs);
        bool answered = false;

        // a short spin catches the fast plugins; then sleep until the child wakes us (macOS 14.4+),
        // so a long hop (De-Feedback) does not burn this core while the child computes it
        const auto spinLimit = juce::jmin (limit, (juce::int64) (spinMicros.load (std::memory_order_relaxed) * 1.0e-3 * ticksPerMs));
        const bool canSleep = canSleepOnAddress() && spinLimit < limit;

        while (true)
        {
            const auto seen = s.done.load (std::memory_order_acquire);
            if (seen == seq) { answered = true; break; }

            const auto elapsed = juce::Time::getHighResolutionTicks() - t0;
            if (elapsed > limit) break;

            if (! canSleep || elapsed < spinLimit)
            {
                rt::cpuPause();
                continue;
            }

            s.hostSleeping.store (1, std::memory_order_seq_cst);
            if (s.done.load (std::memory_order_seq_cst) == seen)
                sleepWhileDoneIs (s, seen, (std::uint64_t) juce::jmax ((juce::int64) 1000, (juce::int64) ((double) (limit - elapsed) / ticksPerMs * 1.0e6)));
            s.hostSleeping.store (0, std::memory_order_relaxed);
        }

        lastRoundTripMs.store ((double) (juce::Time::getHighResolutionTicks() - t0) / ticksPerMs, std::memory_order_relaxed);

        if (! answered)
        {
            lateBlocks.fetch_add (1, std::memory_order_relaxed);
            inFlight = seq;
            juce::AudioBuffer<float> rest (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), start, total - start);
            passThrough (rest);
            return;
        }

        for (int c = 0; c < juce::jmin (numOuts, buffer.getNumChannels()); ++c)
            std::memcpy (buffer.getWritePointer (c, start), s.audioOut[c], sizeof (float) * (size_t) n);
    }
}

//==============================================================================
void SandboxedPlugin::pushParameter (int index, float value)
{
    if (auto* l = active.load())
    {
        const juce::SpinLock::ScopedLockType sl (paramLock);
        l->channel->get().pushChange (index, value);
    }
}

juce::String SandboxedPlugin::parameterText (int index) const
{
    auto* l = active.load();
    if (l == nullptr || ! juce::isPositiveAndBelow (index, Shared::maxParams))
        return {};

    auto& s = l->channel->get();
    char tmp[Shared::textSize];
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto v0 = s.textVersion[index].load (std::memory_order_acquire);
        if ((v0 & 1u) != 0) continue;
        std::memcpy (tmp, s.texts[index], Shared::textSize);
        if (s.textVersion[index].load (std::memory_order_acquire) == v0)
        {
            tmp[Shared::textSize - 1] = 0;
            return juce::String::fromUTF8 (tmp);
        }
    }
    return {};
}

float SandboxedPlugin::getChildProcessMs() const noexcept
{
    auto* l = active.load();
    return l != nullptr ? l->channel->get().processMs.load() : 0.0f;
}

void SandboxedPlugin::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto* l = active.load(); l != nullptr && alive.load())
        if (const auto r = l->call (juce::ValueTree ("getState"), 5000); r.isValid())
            if (auto* data = r["state"].getBinaryData())
            {
                const juce::ScopedLock sl (stateLock);
                lastState = *data;
            }

    const juce::ScopedLock sl (stateLock);
    dest = lastState;   // child gone: the last state it gave us
}

void SandboxedPlugin::setStateInformation (const void* data, int size)
{
    {
        const juce::ScopedLock sl (stateLock);
        lastState = juce::MemoryBlock (data, (size_t) size);
    }

    if (auto* l = active.load(); l != nullptr && alive.load())
    {
        juce::ValueTree m ("setState");
        m.setProperty ("state", juce::MemoryBlock (data, (size_t) size), nullptr);
        l->call (m, 5000);
    }
}

juce::AudioProcessorEditor* SandboxedPlugin::createEditor()
{
    return new SandboxEditor (*this);
}

void SandboxedPlugin::showChildEditor()
{
    if (auto* l = active.load()) l->post (juce::ValueTree ("showEditor"));
}

void SandboxedPlugin::hideChildEditor()
{
    if (auto* l = active.load()) l->post (juce::ValueTree ("hideEditor"));
}

void SandboxedPlugin::crashChildForTests()
{
    if (auto* l = active.load()) l->post (juce::ValueTree ("crashForTests"));
}

void SandboxedPlugin::hangChildForTests (int ms)
{
    juce::ValueTree m ("hangForTests");
    m.setProperty ("ms", ms, nullptr);
    if (auto* l = active.load()) l->post (m);
}

//==============================================================================
void SandboxedPlugin::timerCallback()
{
    auto* l = active.load();
    if (l == nullptr || ! alive.load())
        return;

    // parameter values changed in the child (its editor, its own automation)
    auto& s = l->channel->get();
    for (auto* p : mirrored)
        p->sync (s.values[p->index].load (std::memory_order_relaxed));

    // every 5 s: keep a recent state, to restart the child where it was if it crashes
    if (++ticks % 150 == 0)
    {
        std::weak_ptr<bool> weak = aliveToken;
        juce::Thread::launch ([this, weak, keep = link]
        {
            const auto r = keep->call (juce::ValueTree ("getState"), 5000);
            if (! r.isValid() || r["state"].getBinaryData() == nullptr)
                return;
            // applied on the message thread, where the plugin can't be deleted meanwhile
            juce::MessageManager::callAsync ([this, weak, keep, data = *r["state"].getBinaryData()]
            {
                if (weak.lock() == nullptr || active.load() != keep.get())
                    return;
                const juce::ScopedLock sl (stateLock);
                lastState = data;
            });
        });
    }
}

void SandboxedPlugin::childLost()
{
    if (destroying.load() || restarting.load())
        return;
    alive = false;   // the audio thread passes dry from now on
    restart();
}

void SandboxedPlugin::restart()
{
    restarting = true;
    restarts.fetch_add (1);

    juce::MemoryBlock state;
    {
        const juce::ScopedLock sl (stateLock);
        state = lastState;
    }

    const auto rate = preparedRate > 0 ? preparedRate : getSampleRate();
    const auto block = preparedBlock > 0 ? preparedBlock : getBlockSize();
    std::weak_ptr<bool> weak = aliveToken;
    const auto desc = description;

    juce::Thread::launch ([this, weak, desc, rate, block, state]
    {
        juce::String error;
        auto fresh = std::make_unique<Link>();
        juce::ValueTree r;

        if (fresh->start (error))
            r = fresh->call (loadMessage (desc, *fresh, rate, block, state), loadTimeoutMs);

        auto shared = std::make_shared<std::unique_ptr<Link>> (std::move (fresh));
        juce::MessageManager::callAsync ([this, weak, r, shared]
        {
            if (weak.lock() == nullptr)
                return;

            restarting = false;

            if (! r.isValid() || r.hasProperty ("error"))
            {
                // could not come back: stays dry; try again in 3 s
                juce::Timer::callAfterDelay (3000, [this, weak] { if (weak.lock() != nullptr && ! alive.load()) restart(); });
                return;
            }

            auto& fresh = *shared;
            fresh->onLost = link->onLost;
            fresh->onEvent = link->onEvent;
            link->onLost = nullptr;
            deadLinks.push_back (std::move (link));   // a late audio block may still read it: freed with the plugin
            link = std::move (fresh);
            inFlight = 0;
            active.store (link.get(), std::memory_order_release);
            alive = true;
            setLatencySamples (r["latency"]);
        });
    });
}

} // namespace stagerack::sandbox
