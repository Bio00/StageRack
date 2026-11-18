#include "SandboxHost.h"
#include "SandboxShared.h"
#include "../PluginManager.h"
#include "../../Engine/Rack.h"

namespace stagerack::sandbox
{

namespace
{

class Worker;

/** The plugin's play head in the child: what the host wrote for this block. */
struct SharedPlayHead final : juce::AudioPlayHead
{
    Shared* s = nullptr;

    juce::Optional<PositionInfo> getPosition() const override
    {
        if (s == nullptr || s->hasPosition == 0)
            return {};
        PositionInfo info;
        info.setBpm (s->bpm);
        info.setPpqPosition (s->ppq);
        info.setTimeInSeconds (s->timeSec);
        info.setIsPlaying (s->playing != 0);
        info.setTimeSignature (TimeSignature { s->timeSigNum, s->timeSigDen });
        return info;
    }
};

/** Realtime thread: one block per request from the host. */
class AudioThread final : public juce::Thread
{
public:
    AudioThread (Channel& c, juce::AudioPluginInstance& p, juce::SpinLock& pl, juce::SpinLock& cl,
                 juce::Array<juce::AudioProcessorParameter*> params)
        : juce::Thread ("StageRack sandbox audio"), channel (c), plugin (p), processLock (pl), consumeLock (cl), parameters (params) {}

    std::atomic<double> lastBlockMs { 0.0 };

    void run() override
    {
        auto& s = channel.get();
        std::uint32_t last = s.done.load();
        juce::MidiBuffer midi;
        midi.ensureSize (2048);

        while (! threadShouldExit())
        {
            channel.wait();
            if (threadShouldExit() || s.quit.load() != 0)
                break;

            const auto seq = s.request.load (std::memory_order_acquire);
            if (seq == last)
                continue;

            const juce::ScopedNoDenormals noDenormals;
            const auto t0 = juce::Time::getHighResolutionTicks();
            {
                const juce::SpinLock::ScopedLockType sl (processLock);

                if (consumeLock.tryEnter())
                {
                    s.popChanges ([this] (int i, float v) { if (auto* prm = parameters[i]) prm->setValue (v); });
                    consumeLock.exit();
                }

                const int n = juce::jlimit (0, Shared::maxBlock, s.numSamples.load());
                const int ins = juce::jlimit (0, Shared::maxChannels, s.numInputs.load());
                const int outs = juce::jlimit (0, Shared::maxChannels, s.numOutputs.load());
                const int chans = juce::jmax (ins, outs);
                float* ptrs[Shared::maxChannels];

                for (int c = 0; c < chans; ++c)
                {
                    ptrs[c] = s.audioOut[c];
                    if (c < ins) std::memcpy (s.audioOut[c], s.audioIn[c], sizeof (float) * (size_t) n);
                    else         std::memset (s.audioOut[c], 0, sizeof (float) * (size_t) n);
                }

                juce::AudioBuffer<float> buffer (ptrs, chans, n);
                midi.clear();

                if (n > 0)
                {
                    const juce::ScopedLock cl (plugin.getCallbackLock());
                    if (plugin.isSuspended()) {}
                    else if (s.bypassed.load() != 0) plugin.processBlockBypassed (buffer, midi);
                    else                             plugin.processBlock (buffer, midi);
                }
            }

            s.processMs.store ((float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0),
                               std::memory_order_relaxed);
            last = seq;
            s.done.store (seq, std::memory_order_seq_cst);
            wakeHost (s);
            lastBlockMs.store (juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
        }
    }

private:
    Channel& channel;
    juce::AudioPluginInstance& plugin;
    juce::SpinLock& processLock;
    juce::SpinLock& consumeLock;
    juce::Array<juce::AudioProcessorParameter*> parameters;
};

class EditorWindow final : public juce::DocumentWindow
{
public:
    EditorWindow (const juce::String& title, juce::AudioProcessorEditor* editor)
        : juce::DocumentWindow (title + "  [isolated]", juce::Colours::black, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true);
        setContentNonOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        setAlwaysOnTop (true);
        toFront (true);
        juce::Process::makeForegroundProcess();
    }

    std::function<void()> onClose;
    void closeButtonPressed() override { if (onClose) onClose(); }
};

class Worker final : public juce::ChildProcessWorker,
                     private juce::Timer
{
public:
    void handleMessageFromCoordinator (const juce::MemoryBlock& mb) override
    {
        auto msg = unpack (mb);
        juce::MessageManager::callAsync ([this, msg] { handle (msg); });
    }

    void handleConnectionLost() override
    {
        // the host is gone (quit or crashed): leave at once, without unloading the plugin
        std::_Exit (0);
    }

private:
    juce::AudioPluginFormatManager formats;
    std::unique_ptr<Channel> channel;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<AudioThread> audio;
    std::unique_ptr<EditorWindow> window;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    SharedPlayHead playHead;
    juce::SpinLock processLock, consumeLock;
    juce::Array<juce::AudioProcessorParameter*> params;
    std::vector<float> textShownFor;
    int ticks = 0, latency = -1;

    void reply (const juce::ValueTree& request, juce::ValueTree r)
    {
        r.setProperty ("id", request["id"], nullptr);
        sendMessageToCoordinator (pack (r));
    }

    void startAudio (double sr, int bs)
    {
        audio = std::make_unique<AudioThread> (*channel, *plugin, processLock, consumeLock, params);
        audio->startRealtimeThread (juce::Thread::RealtimeOptions{}.withPeriodMs (1000.0 * bs / juce::jmax (1.0, sr)));
    }

    void stopAudio()
    {
        if (audio == nullptr) return;
        audio->signalThreadShouldExit();
        channel->signal();
        audio->stopThread (2000);
        audio.reset();
    }

    void handle (const juce::ValueTree& msg)
    {
        const auto type = msg.getType().toString();
        juce::ValueTree r ("Reply");

        if (type == "load")
        {
            PluginManager::addAllFormats (formats);
            channel = Channel::open (msg["channel"].toString());
            juce::PluginDescription desc;
            if (auto xml = juce::parseXML (msg["desc"].toString())) desc.loadFromXml (*xml);
            const double sr = msg["sr"];
            const int bs = msg["bs"];
            juce::String error;

            if (channel == nullptr)
                error = "cannot open the shared memory";
            else
                plugin = formats.createPluginInstance (desc, sr, bs, error);

            if (plugin == nullptr)
            {
                r.setProperty ("error", error.isEmpty() ? juce::String ("cannot create the plugin") : error, nullptr);
                reply (msg, r);
                return;
            }

            Rack::configurePlugin (*plugin, sr, bs);
            if (auto* data = msg["state"].getBinaryData(); data != nullptr && data->getSize() > 0)
                plugin->setStateInformation (data->getData(), (int) data->getSize());

            playHead.s = &channel->get();
            plugin->setPlayHead (&playHead);
            params = plugin->getParameters();
            textShownFor.assign ((size_t) params.size(), -1.0f);
            channel->get().numInputs = plugin->getTotalNumInputChannels();
            channel->get().numOutputs = plugin->getTotalNumOutputChannels();
            channel->get().latency = plugin->getLatencySamples();
            latency = plugin->getLatencySamples();

            r.setProperty ("name", plugin->getName(), nullptr);
            r.setProperty ("ins", plugin->getTotalNumInputChannels(), nullptr);
            r.setProperty ("outs", plugin->getTotalNumOutputChannels(), nullptr);
            r.setProperty ("latency", plugin->getLatencySamples(), nullptr);
            r.setProperty ("tail", plugin->getTailLengthSeconds(), nullptr);
            r.setProperty ("hasEditor", plugin->hasEditor(), nullptr);

            for (int i = 0; i < juce::jmin (params.size(), Shared::maxParams); ++i)
            {
                auto* p = params[i];
                juce::ValueTree pt ("P");
                pt.setProperty ("name", p->getName (128), nullptr);
                pt.setProperty ("label", p->getLabel(), nullptr);
                pt.setProperty ("def", p->getDefaultValue(), nullptr);
                pt.setProperty ("value", p->getValue(), nullptr);
                pt.setProperty ("steps", p->getNumSteps(), nullptr);
                pt.setProperty ("discrete", p->isDiscrete(), nullptr);
                pt.setProperty ("bool", p->isBoolean(), nullptr);
                pt.setProperty ("auto", p->isAutomatable(), nullptr);
                pt.setProperty ("meta", p->isMetaParameter(), nullptr);
                pt.setProperty ("text", p->getCurrentValueAsText(), nullptr);
                if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p))
                    pt.setProperty ("pid", hosted->getParameterID(), nullptr);
                if (p->isDiscrete())
                    pt.setProperty ("strings", p->getAllValueStrings().joinIntoString ("\n"), nullptr);
                r.appendChild (pt, nullptr);
                channel->get().values[i].store (p->getValue());
            }

            publish (true);
            startAudio (sr, bs);
            startTimerHz (30);
            reply (msg, r);
            return;
        }

        if (plugin == nullptr)
        {
            r.setProperty ("error", "no plugin", nullptr);
            reply (msg, r);
            return;
        }

        if (type == "prepare")
        {
            const double sr = msg["sr"];
            const int bs = msg["bs"];
            stopAudio();
            plugin->releaseResources();
            plugin->setRateAndBufferSizeDetails (sr, bs);
            plugin->prepareToPlay (sr, bs);
            startAudio (sr, bs);
            r.setProperty ("latency", plugin->getLatencySamples(), nullptr);
        }
        else if (type == "release")
        {
            const juce::SpinLock::ScopedLockType sl (processLock);
            plugin->releaseResources();
        }
        else if (type == "reset")
        {
            const juce::SpinLock::ScopedLockType sl (processLock);
            plugin->reset();
        }
        else if (type == "getState")
        {
            juce::MemoryBlock mb;
            plugin->getStateInformation (mb);
            r.setProperty ("state", mb, nullptr);
        }
        else if (type == "setState")
        {
            if (auto* data = msg["state"].getBinaryData())
                plugin->setStateInformation (data->getData(), (int) data->getSize());
            publish (true);
        }
        else if (type == "showEditor")
        {
            if (window == nullptr && plugin->hasEditor())
            {
                editor.reset (plugin->createEditorIfNeeded());
                if (editor != nullptr)
                {
                    window = std::make_unique<EditorWindow> (plugin->getName(), editor.get());
                    window->onClose = [this] { closeEditor(); };
                }
            }
            else if (window != nullptr)
            {
                window->toFront (true);
                juce::Process::makeForegroundProcess();
            }
        }
        else if (type == "hideEditor")
        {
            closeEditor();
        }
        else if (type == "crashForTests")
        {
            juce::Thread::launch ([] { volatile int* p = nullptr; *p = 1; });   // simulate a plugin crash
            return;
        }
        else if (type == "hangForTests")
        {
            const juce::SpinLock::ScopedLockType sl (processLock);   // simulate a plugin stuck in its audio code
            juce::Thread::sleep (msg["ms"]);
        }

        reply (msg, r);
    }

    void closeEditor()
    {
        if (window != nullptr) window->setVisible (false);
        window.reset();
        if (plugin != nullptr && editor != nullptr) plugin->editorBeingDeleted (editor.get());
        editor.reset();
    }

    void publish (bool allTexts)
    {
        auto& s = channel->get();

        for (int i = 0; i < juce::jmin (params.size(), Shared::maxParams); ++i)
        {
            const auto v = params[i]->getValue();
            s.values[i].store (v, std::memory_order_relaxed);

            if (allTexts || v != textShownFor[(size_t) i])
            {
                textShownFor[(size_t) i] = v;
                const auto text = params[i]->getCurrentValueAsText();
                char tmp[Shared::textSize] {};
                text.copyToUTF8 (tmp, Shared::textSize - 1);
                s.textVersion[i].fetch_add (1, std::memory_order_acq_rel);   // odd = writing
                std::memcpy (s.texts[i], tmp, Shared::textSize);
                s.textVersion[i].fetch_add (1, std::memory_order_release);
            }
        }
    }

    void timerCallback() override
    {
        auto& s = channel->get();

        // no audio for a while (device stopped, offline): the parameter changes still apply
        if (audio == nullptr || juce::Time::getMillisecondCounterHiRes() - audio->lastBlockMs.load() > 50.0)
        {
            const juce::SpinLock::ScopedLockType cl (consumeLock);
            s.popChanges ([this] (int i, float v) { if (auto* prm = params[i]) prm->setValue (v); });
        }

        publish (false);

        if (const auto l = plugin->getLatencySamples(); l != latency)
        {
            latency = l;
            s.latency = l;
            juce::ValueTree ev ("latency");
            ev.setProperty ("latency", l, nullptr);
            sendMessageToCoordinator (pack (ev));
        }
    }
};

} // namespace

bool isSandboxCommandLine (const juce::String& commandLine)
{
    return commandLine.contains (juce::String ("--") + commandLineId + ":");
}

bool startSandboxChild (const juce::String& commandLine)
{
    static std::unique_ptr<Worker> worker;   // lives until the process exits (_Exit)
    worker = std::make_unique<Worker>();

    if (worker->initialiseFromCommandLine (commandLine, commandLineId, 10000))
        return true;

    worker.reset();
    return false;
}

} // namespace stagerack::sandbox
