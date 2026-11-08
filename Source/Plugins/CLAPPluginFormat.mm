#import <Cocoa/Cocoa.h>
#include <CoreFoundation/CoreFoundation.h>
#include <clap/clap.h>

#include "CLAPPluginFormat.h"
#include <map>
#include <mutex>

namespace stagerack
{

//==============================================================================
/** A loaded .clap bundle. Shared between all instances that come from it. */
class CLAPModule
{
public:
    static std::shared_ptr<CLAPModule> load (const juce::String& path)
    {
        static std::mutex lock;
        static std::map<juce::String, std::weak_ptr<CLAPModule>> cache;

        const std::lock_guard<std::mutex> sl (lock);

        if (auto existing = cache[path].lock())
            return existing;

        auto m = std::shared_ptr<CLAPModule> (new CLAPModule (path));

        if (m->entry == nullptr)
            return {};

        cache[path] = m;
        return m;
    }

    ~CLAPModule()
    {
        if (entry != nullptr)
            entry->deinit();

        if (bundle != nullptr)
            CFRelease (bundle);
    }

    const clap_plugin_factory_t* getFactory() const
    {
        return entry != nullptr ? static_cast<const clap_plugin_factory_t*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID))
                                : nullptr;
    }

private:
    explicit CLAPModule (const juce::String& path)
    {
        auto url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.toRawUTF8(),
                                                            (CFIndex) strlen (path.toRawUTF8()), true);
        if (url == nullptr)
            return;

        bundle = CFBundleCreate (nullptr, url);
        CFRelease (url);

        if (bundle == nullptr || ! CFBundleLoadExecutable (bundle))
            return;

        auto* e = static_cast<const clap_plugin_entry_t*> (CFBundleGetDataPointerForName (bundle, CFSTR ("clap_entry")));

        if (e == nullptr || ! clap_version_is_compatible (e->clap_version))
            return;

        if (e->init (path.toRawUTF8()))
            entry = e;
    }

    CFBundleRef bundle = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
};

static int hashPluginId (const char* id)
{
    return (int) juce::String (id).hashCode();
}

static bool hasFeature (const clap_plugin_descriptor_t* d, const char* feature)
{
    if (d->features == nullptr)
        return false;

    for (auto f = d->features; *f != nullptr; ++f)
        if (strcmp (*f, feature) == 0)
            return true;

    return false;
}

//==============================================================================
class CLAPPluginInstance;

class CLAPParameter final : public juce::HostedAudioProcessorParameter
{
public:
    CLAPParameter (CLAPPluginInstance& o, const clap_param_info_t& info, double currentValue)
        : owner (o), id (info.id), cookie (info.cookie), flags (info.flags),
          minValue (info.min_value), maxValue (info.max_value), defaultValue (info.default_value),
          name (juce::String::fromUTF8 (info.name))
    {
        plainValue.store (currentValue);
    }

    float getValue() const override              { return toNormalised (plainValue.load (std::memory_order_relaxed)); }
    void setValue (float newValue) override;
    float getDefaultValue() const override       { return toNormalised (defaultValue); }
    juce::String getName (int maxLen) const override { return name.substring (0, maxLen); }
    juce::String getLabel() const override       { return {}; }
    juce::String getText (float normalised, int maxLen) const override;
    float getValueForText (const juce::String& text) const override;
    juce::String getParameterID() const override { return juce::String ((juce::int64) id); }
    bool isAutomatable() const override          { return (flags & CLAP_PARAM_IS_AUTOMATABLE) != 0; }
    bool isDiscrete() const override             { return (flags & CLAP_PARAM_IS_STEPPED) != 0; }
    bool isBoolean() const override              { return isDiscrete() && (maxValue - minValue) == 1.0; }
    int getNumSteps() const override
    {
        return isDiscrete() ? juce::jmax (2, (int) (maxValue - minValue) + 1) : juce::AudioProcessor::getDefaultNumParameterSteps();
    }

    float toNormalised (double plain) const noexcept
    {
        return maxValue > minValue ? (float) juce::jlimit (0.0, 1.0, (plain - minValue) / (maxValue - minValue)) : 0.0f;
    }

    double toPlain (float normalised) const noexcept
    {
        auto v = minValue + (double) normalised * (maxValue - minValue);
        return isDiscrete() ? std::round (v) : v;
    }

    CLAPPluginInstance& owner;
    const clap_id id;
    void* const cookie;
    const clap_param_info_flags flags;
    const double minValue, maxValue, defaultValue;
    const juce::String name;
    std::atomic<double> plainValue { 0.0 };
};

//==============================================================================
/** Fixed-capacity CLAP input event list (no allocations on the audio thread). */
struct EventList
{
    union Event
    {
        clap_event_header_t header;
        clap_event_param_value_t param;
        clap_event_midi_t midi;
    };

    static constexpr uint32_t capacity = 1024;

    EventList()
    {
        events.resize (capacity);
        in.ctx = this;
        in.size = [] (const clap_input_events_t* l) -> uint32_t { return static_cast<EventList*> (l->ctx)->count; };
        in.get = [] (const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t*
        {
            auto* self = static_cast<EventList*> (l->ctx);
            return i < self->count ? &self->events[i].header : nullptr;
        };
    }

    Event* add() noexcept    { return count < capacity ? &events[count++] : nullptr; }
    void clear() noexcept    { count = 0; }

    std::vector<Event> events;
    uint32_t count = 0;
    clap_input_events_t in {};
};

//==============================================================================
class CLAPPluginInstance final : public juce::AudioPluginInstance,
                                 private juce::AsyncUpdater
{
public:
    struct PluginTimer final : public juce::Timer
    {
        PluginTimer (CLAPPluginInstance& o, clap_id i) : owner (o), id (i) {}
        void timerCallback() override
        {
            if (owner.timerExt != nullptr)
                owner.timerExt->on_timer (owner.plugin, id);
        }
        CLAPPluginInstance& owner;
        const clap_id id;
    };

    struct PortLayout
    {
        std::vector<uint32_t> inputs, outputs;  // channel count per port
    };

    static std::unique_ptr<CLAPPluginInstance> create (const juce::PluginDescription& desc, juce::String& error)
    {
        auto module = CLAPModule::load (desc.fileOrIdentifier);

        if (module == nullptr)
        {
            error = "Could not load CLAP bundle";
            return {};
        }

        auto* factory = module->getFactory();

        if (factory == nullptr)
        {
            error = "CLAP bundle has no plugin factory";
            return {};
        }

        const clap_plugin_descriptor_t* found = nullptr;

        for (uint32_t i = 0; i < factory->get_plugin_count (factory); ++i)
            if (auto* d = factory->get_plugin_descriptor (factory, i); d != nullptr && hashPluginId (d->id) == desc.uniqueId)
                found = d;

        if (found == nullptr)
        {
            error = "CLAP plugin id not found in bundle";
            return {};
        }

        auto host = std::make_unique<HostData>();
        const auto* plugin = factory->create_plugin (factory, &host->host, found->id);

        if (plugin == nullptr)
        {
            error = "CLAP plugin creation failed";
            return {};
        }

        if (! plugin->init (plugin))
        {
            plugin->destroy (plugin);
            error = "CLAP plugin init failed";
            return {};
        }

        PortLayout ports;

        if (auto* ap = static_cast<const clap_plugin_audio_ports_t*> (plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS)))
        {
            for (int dir = 0; dir < 2; ++dir)
            {
                const bool isInput = dir == 0;

                for (uint32_t i = 0; i < ap->count (plugin, isInput); ++i)
                {
                    clap_audio_port_info_t info {};
                    if (ap->get (plugin, i, isInput, &info))
                        (isInput ? ports.inputs : ports.outputs).push_back (info.channel_count);
                }
            }
        }
        else
        {
            ports.inputs = { 2 };
            ports.outputs = { 2 };
        }

        return std::unique_ptr<CLAPPluginInstance> (new CLAPPluginInstance (std::move (module), std::move (host),
                                                                            plugin, desc, std::move (ports)));
    }

    ~CLAPPluginInstance() override
    {
        cancelPendingUpdate();
        *alive = false;
        timers.clear();

        if (active)
        {
            if (processing)
                plugin->stop_processing (plugin);

            plugin->deactivate (plugin);
        }

        plugin->destroy (plugin);
    }

    //==============================================================================
    const juce::String getName() const override              { return description.name; }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d = description; }
    double getTailLengthSeconds() const override             { return 0.0; }
    bool acceptsMidi() const override                        { return description.isInstrument; }
    bool producesMidi() const override                       { return false; }
    bool supportsDoublePrecisionProcessing() const override  { return false; }

    int getNumPrograms() override                            { return 1; }
    int getCurrentProgram() override                         { return 0; }
    void setCurrentProgram (int) override                    {}
    const juce::String getProgramName (int) override         { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        // CLAP ports have a fixed channel count unless the plugin supports configs.
        for (size_t i = 0; i < ports.inputs.size() && i < (size_t) layout.inputBuses.size(); ++i)
            if ((uint32_t) layout.inputBuses[(int) i].size() != ports.inputs[i])
                return false;

        for (size_t i = 0; i < ports.outputs.size() && i < (size_t) layout.outputBuses.size(); ++i)
            if ((uint32_t) layout.outputBuses[(int) i].size() != ports.outputs[i])
                return false;

        return true;
    }

    //==============================================================================
    void prepareToPlay (double sampleRate, int maxBlock) override
    {
        const juce::ScopedLock sl (getCallbackLock());
        deactivateLocked();

        maxBlockSize = juce::jmax (1, maxBlock);

        auto allocPorts = [this] (const std::vector<uint32_t>& counts, std::vector<juce::AudioBuffer<float>>& scratch,
                                  std::vector<std::vector<float*>>& ptrs, std::vector<clap_audio_buffer_t>& bufs)
        {
            scratch.clear();
            ptrs.clear();
            bufs.clear();

            for (auto c : counts)
            {
                scratch.emplace_back ((int) juce::jmax (1u, c), maxBlockSize);
                ptrs.emplace_back ((size_t) c, nullptr);
            }

            bufs.resize (counts.size());

            for (size_t i = 0; i < counts.size(); ++i)
            {
                auto& b = bufs[i];
                b = {};
                b.channel_count = counts[i];
                b.data32 = ptrs[i].data();
            }
        };

        allocPorts (ports.inputs, inScratch, inPtrs, inBufs);
        allocPorts (ports.outputs, outScratch, outPtrs, outBufs);

        active = plugin->activate (plugin, sampleRate, 1, (uint32_t) maxBlockSize);
        processing = false;

        if (latencyExt != nullptr && active)
            setLatencySamples ((int) latencyExt->get (plugin));
    }

    void releaseResources() override
    {
        const juce::ScopedLock sl (getCallbackLock());
        deactivateLocked();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        const auto numSamples = buffer.getNumSamples();

        if (! active || numSamples > maxBlockSize)
        {
            buffer.clear();
            return;
        }

        if (! processing)
            processing = plugin->start_processing (plugin);

        // --- events: queued parameter changes + MIDI
        inEvents.clear();
        drainParamQueue (inEvents);

        for (const auto m : midi)
        {
            const auto& msg = m.getMessage();

            if (msg.getRawDataSize() > 3)
                continue;

            if (auto* e = inEvents.add())
            {
                e->midi = {};
                e->midi.header = { sizeof (clap_event_midi_t), (uint32_t) m.samplePosition, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
                e->midi.port_index = 0;
                for (int i = 0; i < msg.getRawDataSize(); ++i)
                    e->midi.data[i] = msg.getRawData()[i];
            }
        }

        midi.clear();

        // --- audio: map the JUCE buffer channels to CLAP ports (main ports first)
        int ch = 0;

        for (size_t p = 0; p < inBufs.size(); ++p)
        {
            for (uint32_t c = 0; c < ports.inputs[p]; ++c, ++ch)
            {
                auto* dest = inScratch[p].getWritePointer ((int) c);

                if (ch < buffer.getNumChannels())
                    juce::FloatVectorOperations::copy (dest, buffer.getReadPointer (ch), numSamples);
                else
                    juce::FloatVectorOperations::clear (dest, numSamples);

                inPtrs[p][c] = dest;
            }
        }

        ch = 0;

        for (size_t p = 0; p < outBufs.size(); ++p)
            for (uint32_t c = 0; c < ports.outputs[p]; ++c, ++ch)
                outPtrs[p][c] = ch < buffer.getNumChannels() ? buffer.getWritePointer (ch)
                                                             : outScratch[p].getWritePointer ((int) c);

        clap_process_t proc {};
        proc.steady_time = steadyTime;
        proc.frames_count = (uint32_t) numSamples;
        proc.audio_inputs = inBufs.data();
        proc.audio_inputs_count = (uint32_t) inBufs.size();
        proc.audio_outputs = outBufs.data();
        proc.audio_outputs_count = (uint32_t) outBufs.size();
        proc.in_events = &inEvents.in;
        proc.out_events = &outEvents;

        if (plugin->process (plugin, &proc) == CLAP_PROCESS_ERROR)
            buffer.clear();

        // channels the plugin doesn't output must not keep the input signal
        for (int c = ch; c < buffer.getNumChannels(); ++c)
            buffer.clear (c, 0, numSamples);

        steadyTime += numSamples;
    }

    //==============================================================================
    bool hasEditor() const override
    {
        return guiExt != nullptr && guiExt->is_api_supported (plugin, CLAP_WINDOW_API_COCOA, false);
    }

    juce::AudioProcessorEditor* createEditor() override;

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        if (stateExt == nullptr)
            return;

        juce::MemoryOutputStream mos (dest, false);
        clap_ostream_t os { &mos, [] (const clap_ostream_t* s, const void* data, uint64_t size) -> int64_t
        {
            return static_cast<juce::MemoryOutputStream*> (s->ctx)->write (data, (size_t) size) ? (int64_t) size : -1;
        } };

        stateExt->save (plugin, &os);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (stateExt == nullptr)
            return;

        juce::MemoryInputStream mis (data, (size_t) size, false);
        clap_istream_t is { &mis, [] (const clap_istream_t* s, void* buffer, uint64_t sz) -> int64_t
        {
            return static_cast<juce::MemoryInputStream*> (s->ctx)->read (buffer, (int) sz);
        } };

        stateExt->load (plugin, &is);
        refreshParamValues();
    }

    //==============================================================================
    void queueParamValue (CLAPParameter& p, double plain)
    {
        {
            const juce::SpinLock::ScopedLockType sl (queueLock);
            int start1, size1, start2, size2;
            paramFifo.prepareToWrite (1, start1, size1, start2, size2);

            if (size1 > 0)
            {
                paramQueue[(size_t) start1] = { p.id, p.cookie, plain };
                paramFifo.finishedWrite (1);
            }
        }

        if (! processing)
            triggerAsyncUpdate(); // flush on the main thread
    }

    bool valueToText (clap_id id, double v, char* out, uint32_t cap) const
    {
        return paramsExt != nullptr && paramsExt->value_to_text (plugin, id, v, out, cap);
    }

    bool textToValue (clap_id id, const char* text, double* out) const
    {
        return paramsExt != nullptr && paramsExt->text_to_value (plugin, id, text, out);
    }

    const clap_plugin_t* const plugin;
    const clap_plugin_gui_t* guiExt = nullptr;

    // GUI callbacks from the plugin (handled by the editor)
    std::function<void (uint32_t, uint32_t)> onGuiResizeRequest;
    std::function<void()> onGuiClosed;

    //==============================================================================
    // Host extension entry points (called by the static clap_host_* tables)
    static void hostLatencyChanged (const clap_host_t* h)
    {
        post (h, [] (CLAPPluginInstance& self)
        {
            if (self.latencyExt != nullptr && self.active)
                self.setLatencySamples ((int) self.latencyExt->get (self.plugin));
        });
    }

    static void hostGuiResize (const clap_host_t* h, uint32_t w, uint32_t hgt)
    {
        post (h, [w, hgt] (CLAPPluginInstance& self)
        {
            if (self.onGuiResizeRequest)
                self.onGuiResizeRequest (w, hgt);
        });
    }

    static void hostGuiClosed (const clap_host_t* h)
    {
        post (h, [] (CLAPPluginInstance& self)
        {
            if (self.onGuiClosed)
                self.onGuiClosed();
        });
    }

    static bool hostRegisterTimer (const clap_host_t* h, uint32_t periodMs, clap_id* outId)
    {
        auto* self = from (h);

        if (self == nullptr || ! juce::MessageManager::existsAndIsCurrentThread())
            return false;

        const auto id = self->nextTimerId++;
        auto t = std::make_unique<PluginTimer> (*self, id);
        t->startTimer ((int) juce::jmax (1u, periodMs));
        self->timers[id] = std::move (t);
        *outId = id;
        return true;
    }

    static bool hostUnregisterTimer (const clap_host_t* h, clap_id id)
    {
        auto* self = from (h);

        if (self == nullptr || ! juce::MessageManager::existsAndIsCurrentThread())
            return false;

        return self->timers.erase (id) > 0;
    }

private:
    //==============================================================================
    struct HostData
    {
        HostData()
        {
            host.clap_version = CLAP_VERSION;
            host.host_data = nullptr;
            host.name = "StageRack";
            host.vendor = "bio";
            host.url = "";
            host.version = "0.1";
            host.get_extension = &CLAPPluginInstance::hostGetExtension;
            host.request_restart = &CLAPPluginInstance::hostRequestRestart;
            host.request_process = [] (const clap_host_t*) {};
            host.request_callback = &CLAPPluginInstance::hostRequestCallback;
        }

        clap_host_t host {};
    };

    static CLAPPluginInstance* from (const clap_host_t* h) { return static_cast<CLAPPluginInstance*> (h->host_data); }

    // host_data is still null while the plugin runs create()/init()
    static void post (const clap_host_t* h, std::function<void (CLAPPluginInstance&)> fn)
    {
        if (auto* self = from (h))
            self->runOnMainThread (std::move (fn));
    }

    static juce::AudioProcessor::BusesProperties makeBuses (const PortLayout& ports)
    {
        BusesProperties props;

        auto setFor = [] (uint32_t n)
        {
            return n == 1 ? juce::AudioChannelSet::mono()
                          : (n == 2 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::discreteChannels ((int) n));
        };

        for (size_t i = 0; i < ports.inputs.size(); ++i)
            props.addBus (true, i == 0 ? "Input" : "Input " + juce::String (i + 1), setFor (ports.inputs[i]), true);

        for (size_t i = 0; i < ports.outputs.size(); ++i)
            props.addBus (false, i == 0 ? "Output" : "Output " + juce::String (i + 1), setFor (ports.outputs[i]), true);

        return props;
    }

    CLAPPluginInstance (std::shared_ptr<CLAPModule> m, std::unique_ptr<HostData> h, const clap_plugin_t* p,
                        const juce::PluginDescription& desc, PortLayout portLayout)
        : juce::AudioPluginInstance (makeBuses (portLayout)),
          plugin (p), module (std::move (m)), hostData (std::move (h)),
          description (desc), ports (std::move (portLayout))
    {
        hostData->host.host_data = this;

        guiExt     = static_cast<const clap_plugin_gui_t*>     (plugin->get_extension (plugin, CLAP_EXT_GUI));
        paramsExt  = static_cast<const clap_plugin_params_t*>  (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        stateExt   = static_cast<const clap_plugin_state_t*>   (plugin->get_extension (plugin, CLAP_EXT_STATE));
        latencyExt = static_cast<const clap_plugin_latency_t*> (plugin->get_extension (plugin, CLAP_EXT_LATENCY));
        timerExt   = static_cast<const clap_plugin_timer_support_t*> (plugin->get_extension (plugin, CLAP_EXT_TIMER_SUPPORT));

        outEvents.ctx = this;
        outEvents.try_push = [] (const clap_output_events_t* l, const clap_event_header_t* e) -> bool
        {
            static_cast<CLAPPluginInstance*> (l->ctx)->handleOutputEvent (e);
            return true;
        };

        if (paramsExt != nullptr)
        {
            for (uint32_t i = 0; i < paramsExt->count (plugin); ++i)
            {
                clap_param_info_t info {};

                if (! paramsExt->get_info (plugin, i, &info) || (info.flags & CLAP_PARAM_IS_HIDDEN) != 0)
                    continue;

                double v = info.default_value;
                paramsExt->get_value (plugin, info.id, &v);

                auto param = std::make_unique<CLAPParameter> (*this, info, v);
                paramById[info.id] = param.get();
                addHostedParameter (std::move (param));
            }
        }
    }

    void deactivateLocked()
    {
        if (! active)
            return;

        if (processing)
            plugin->stop_processing (plugin);

        plugin->deactivate (plugin);
        active = processing = false;
    }

    void drainParamQueue (EventList& list) noexcept
    {
        int start1, size1, start2, size2;
        const auto num = paramFifo.getNumReady();
        paramFifo.prepareToRead (num, start1, size1, start2, size2);

        auto push = [&] (int idx)
        {
            const auto& q = paramQueue[(size_t) idx];

            if (auto* e = list.add())
            {
                e->param = {};
                e->param.header = { sizeof (clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
                e->param.param_id = q.id;
                e->param.cookie = q.cookie;
                e->param.note_id = -1;
                e->param.port_index = -1;
                e->param.channel = -1;
                e->param.key = -1;
                e->param.value = q.value;
            }
        };

        for (int i = 0; i < size1; ++i) push (start1 + i);
        for (int i = 0; i < size2; ++i) push (start2 + i);

        paramFifo.finishedRead (size1 + size2);
    }

    void handleOutputEvent (const clap_event_header_t* e) noexcept
    {
        if (e->space_id == CLAP_CORE_EVENT_SPACE_ID && e->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* pv = reinterpret_cast<const clap_event_param_value_t*> (e);

            if (auto it = paramById.find (pv->param_id); it != paramById.end())
                it->second->plainValue.store (pv->value, std::memory_order_relaxed);
        }
    }

    void refreshParamValues()
    {
        if (paramsExt == nullptr)
            return;

        for (auto& [id, p] : paramById)
        {
            double v = 0;
            if (paramsExt->get_value (plugin, id, &v))
                p->plainValue.store (v);
        }
    }

    // Main-thread flush of parameter changes while the plugin isn't processing.
    void handleAsyncUpdate() override
    {
        if (paramsExt == nullptr || processing)
            return;

        EventList list;
        drainParamQueue (list);
        paramsExt->flush (plugin, &list.in, &outEvents);
    }

    void runOnMainThread (std::function<void (CLAPPluginInstance&)> fn)
    {
        std::weak_ptr<bool> weak = alive;

        juce::MessageManager::callAsync ([this, weak, fn = std::move (fn)]
        {
            if (auto a = weak.lock(); a != nullptr && *a)
                fn (*this);
        });
    }

    //==============================================================================
    // Host callbacks
    static const void* CLAP_ABI hostGetExtension (const clap_host_t*, const char* id);
    static void CLAP_ABI hostRequestRestart (const clap_host_t* h)
    {
        post (h, [] (CLAPPluginInstance& self)
        {
            if (! self.active)
                return;

            const auto sr = self.getSampleRate();
            const auto bs = self.getBlockSize();
            self.prepareToPlay (sr, bs);
        });
    }

    static void CLAP_ABI hostRequestCallback (const clap_host_t* h)
    {
        post (h, [] (CLAPPluginInstance& self) { self.plugin->on_main_thread (self.plugin); });
    }

    friend class CLAPEditor;

    std::shared_ptr<CLAPModule> module;
    std::unique_ptr<HostData> hostData;
    juce::PluginDescription description;
    PortLayout ports;

    const clap_plugin_params_t* paramsExt = nullptr;
    const clap_plugin_state_t* stateExt = nullptr;
    const clap_plugin_latency_t* latencyExt = nullptr;
    const clap_plugin_timer_support_t* timerExt = nullptr;

    std::map<clap_id, CLAPParameter*> paramById;
    std::map<clap_id, std::unique_ptr<PluginTimer>> timers;
    clap_id nextTimerId = 1;

    struct QueuedParam { clap_id id; void* cookie; double value; };
    std::array<QueuedParam, 1024> paramQueue {};
    juce::AbstractFifo paramFifo { 1024 };
    juce::SpinLock queueLock;

    EventList inEvents;
    clap_output_events_t outEvents {};

    std::vector<juce::AudioBuffer<float>> inScratch, outScratch;
    std::vector<std::vector<float*>> inPtrs, outPtrs;
    std::vector<clap_audio_buffer_t> inBufs, outBufs;

    int maxBlockSize = 512;
    int64_t steadyTime = 0;
    bool active = false, processing = false;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CLAPPluginInstance)
};

//==============================================================================
void CLAPParameter::setValue (float newValue)
{
    const auto plain = toPlain (newValue);
    plainValue.store (plain, std::memory_order_relaxed);
    owner.queueParamValue (*this, plain);
}

juce::String CLAPParameter::getText (float normalised, int maxLen) const
{
    char buf[256] = {};

    if (owner.valueToText (id, toPlain (normalised), buf, sizeof (buf)))
        return juce::String::fromUTF8 (buf).substring (0, maxLen);

    return juce::String (toPlain (normalised), 2).substring (0, maxLen);
}

float CLAPParameter::getValueForText (const juce::String& text) const
{
    double v = 0;

    if (owner.textToValue (id, text.toRawUTF8(), &v))
        return toNormalised (v);

    return toNormalised (text.getDoubleValue());
}

//==============================================================================
// Host extensions
static const clap_host_log_t hostLog {
    [] (const clap_host_t*, clap_log_severity severity, const char* msg)
    {
        if (severity >= CLAP_LOG_WARNING)
            DBG ("[CLAP] " << msg);
    }
};

static const clap_host_thread_check_t hostThreadCheck {
    [] (const clap_host_t*) { return juce::MessageManager::existsAndIsCurrentThread(); },
    [] (const clap_host_t*) { return ! juce::MessageManager::existsAndIsCurrentThread(); }
};

static const clap_host_params_t hostParams {
    [] (const clap_host_t*, clap_param_rescan_flags) {},
    [] (const clap_host_t*, clap_id, clap_param_clear_flags) {},
    [] (const clap_host_t*) {}
};

static const clap_host_state_t hostState { [] (const clap_host_t*) {} };

static const clap_host_audio_ports_t hostAudioPorts {
    [] (const clap_host_t*, uint32_t) { return false; },
    [] (const clap_host_t*, uint32_t) {}
};

static const clap_host_latency_t hostLatency {
    [] (const clap_host_t* h) { CLAPPluginInstance::hostLatencyChanged (h); }
};

static const clap_host_gui_t hostGui {
    [] (const clap_host_t*) {},
    [] (const clap_host_t* h, uint32_t w, uint32_t hgt)
    {
        CLAPPluginInstance::hostGuiResize (h, w, hgt);
        return true;
    },
    [] (const clap_host_t*) { return false; },
    [] (const clap_host_t*) { return false; },
    [] (const clap_host_t* h, bool) { CLAPPluginInstance::hostGuiClosed (h); }
};

static const clap_host_timer_support_t hostTimers {
    [] (const clap_host_t* h, uint32_t periodMs, clap_id* outId) { return CLAPPluginInstance::hostRegisterTimer (h, periodMs, outId); },
    [] (const clap_host_t* h, clap_id id) { return CLAPPluginInstance::hostUnregisterTimer (h, id); }
};


const void* CLAP_ABI CLAPPluginInstance::hostGetExtension (const clap_host_t*, const char* id)
{
    if (strcmp (id, CLAP_EXT_LOG) == 0)            return &hostLog;
    if (strcmp (id, CLAP_EXT_THREAD_CHECK) == 0)   return &hostThreadCheck;
    if (strcmp (id, CLAP_EXT_PARAMS) == 0)         return &hostParams;
    if (strcmp (id, CLAP_EXT_STATE) == 0)          return &hostState;
    if (strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0)    return &hostAudioPorts;
    if (strcmp (id, CLAP_EXT_LATENCY) == 0)        return &hostLatency;
    if (strcmp (id, CLAP_EXT_GUI) == 0)            return &hostGui;
    if (strcmp (id, CLAP_EXT_TIMER_SUPPORT) == 0)  return &hostTimers;
    return nullptr;
}

//==============================================================================
/** Embeds the plugin's Cocoa view in a JUCE editor. */
class CLAPEditor final : public juce::AudioProcessorEditor
{
public:
    explicit CLAPEditor (CLAPPluginInstance& p)
        : juce::AudioProcessorEditor (p), instance (p), gui (p.guiExt)
    {
        addAndMakeVisible (host);

        container = [[NSView alloc] initWithFrame: NSMakeRect (0, 0, 400, 300)];
        host.setView (container);

        created = gui->create (instance.plugin, CLAP_WINDOW_API_COCOA, false);

        uint32_t w = 400, h = 300;

        if (created)
        {
            clap_window_t window {};
            window.api = CLAP_WINDOW_API_COCOA;
            window.cocoa = (__bridge void*) container;
            gui->set_parent (instance.plugin, &window);
            gui->get_size (instance.plugin, &w, &h);
        }

        resizable = created && gui->can_resize (instance.plugin);
        setResizable (resizable, false);
        applyingPluginSize = true;
        setSize ((int) w, (int) h);
        applyingPluginSize = false;

        instance.onGuiResizeRequest = [this] (uint32_t nw, uint32_t nh)
        {
            applyingPluginSize = true;
            setSize ((int) nw, (int) nh);
            applyingPluginSize = false;
        };

        if (created)
            gui->show (instance.plugin);
    }

    ~CLAPEditor() override
    {
        instance.onGuiResizeRequest = nullptr;

        if (created)
        {
            gui->hide (instance.plugin);
            gui->destroy (instance.plugin);
        }

        host.setView (nullptr);
        [container release];
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

    void resized() override
    {
        host.setBounds (getLocalBounds());

        if (created && resizable && ! applyingPluginSize)
        {
            uint32_t w = (uint32_t) getWidth(), h = (uint32_t) getHeight();
            gui->adjust_size (instance.plugin, &w, &h);
            gui->set_size (instance.plugin, w, h);
        }
        else if (created && applyingPluginSize && resizable)
        {
            gui->set_size (instance.plugin, (uint32_t) getWidth(), (uint32_t) getHeight());
        }
    }

private:
    CLAPPluginInstance& instance;
    const clap_plugin_gui_t* gui;
    juce::NSViewComponent host;
    NSView* container = nil;
    bool created = false, resizable = false, applyingPluginSize = false;
};

juce::AudioProcessorEditor* CLAPPluginInstance::createEditor()
{
    return hasEditor() ? new CLAPEditor (*this) : nullptr;
}

//==============================================================================
// Format
void CLAPPluginFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& path)
{
    if (! fileMightContainThisPluginType (path))
        return;

    auto module = CLAPModule::load (path);

    if (module == nullptr)
        return;

    auto* factory = module->getFactory();

    if (factory == nullptr)
        return;

    const juce::File file (path);

    for (uint32_t i = 0; i < factory->get_plugin_count (factory); ++i)
    {
        auto* d = factory->get_plugin_descriptor (factory, i);

        if (d == nullptr || d->id == nullptr)
            continue;

        auto desc = std::make_unique<juce::PluginDescription>();
        desc->name = juce::String::fromUTF8 (d->name);
        desc->descriptiveName = juce::String::fromUTF8 (d->description != nullptr ? d->description : d->name);
        desc->pluginFormatName = getName();
        desc->manufacturerName = juce::String::fromUTF8 (d->vendor != nullptr ? d->vendor : "");
        desc->version = juce::String::fromUTF8 (d->version != nullptr ? d->version : "");
        desc->fileOrIdentifier = path;
        desc->lastFileModTime = file.getLastModificationTime();
        desc->lastInfoUpdateTime = juce::Time::getCurrentTime();
        desc->uniqueId = desc->deprecatedUid = hashPluginId (d->id);
        desc->isInstrument = hasFeature (d, CLAP_PLUGIN_FEATURE_INSTRUMENT);
        desc->category = desc->isInstrument ? "Instrument" : "Effect";
        desc->numInputChannels = desc->isInstrument ? 0 : 2;
        desc->numOutputChannels = 2;
        desc->hasSharedContainer = factory->get_plugin_count (factory) > 1;
        results.add (desc.release());
    }
}

bool CLAPPluginFormat::fileMightContainThisPluginType (const juce::String& path)
{
    const juce::File f (path);
    return f.hasFileExtension (".clap") && f.exists();
}

juce::String CLAPPluginFormat::getNameOfPluginFromIdentifier (const juce::String& path)
{
    return juce::File (path).getFileNameWithoutExtension();
}

bool CLAPPluginFormat::pluginNeedsRescanning (const juce::PluginDescription& d)
{
    return juce::File (d.fileOrIdentifier).getLastModificationTime() != d.lastFileModTime;
}

bool CLAPPluginFormat::doesPluginStillExist (const juce::PluginDescription& d)
{
    return juce::File (d.fileOrIdentifier).exists();
}

static void findClapBundles (const juce::File& dir, bool recursive, juce::StringArray& out)
{
    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFilesAndDirectories))
    {
        const auto f = entry.getFile();

        if (f.hasFileExtension (".clap"))
            out.addIfNotAlreadyThere (f.getFullPathName());
        else if (recursive && f.isDirectory())
            findClapBundles (f, recursive, out);
    }
}

juce::StringArray CLAPPluginFormat::searchPathsForPlugins (const juce::FileSearchPath& paths, bool recursive, bool)
{
    juce::StringArray result;

    for (int i = 0; i < paths.getNumPaths(); ++i)
        findClapBundles (paths[i], recursive, result);

    return result;
}

juce::FileSearchPath CLAPPluginFormat::getDefaultLocationsToSearch()
{
    return juce::FileSearchPath ("~/Library/Audio/Plug-Ins/CLAP;/Library/Audio/Plug-Ins/CLAP");
}

void CLAPPluginFormat::createPluginInstance (const juce::PluginDescription& desc, double sampleRate,
                                             int blockSize, PluginCreationCallback callback)
{
    juce::String error;
    auto instance = CLAPPluginInstance::create (desc, error);

    if (instance != nullptr)
        instance->setRateAndBufferSizeDetails (sampleRate, blockSize);

    callback (std::move (instance), error);
}

} // namespace stagerack
