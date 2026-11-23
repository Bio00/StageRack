#pragma once
#include <JuceHeader.h>

namespace stagerack::auwg
{
/** Audio Units can ask for the audio workgroup of the thread that renders them
    (kAudioUnitProperty_RenderContextObserver): with it a plugin can make its own
    worker threads join the device's workgroup, so macOS schedules them for the
    deadline. JUCE's AU host never tells them; we do. */

/** The IO workgroup of the CoreAudio device with this name (+1 retained), or nullptr. */
void* copyDeviceWorkgroup (const juce::String& deviceName);
void releaseWorkgroup (void* wg);

/** One instance's observer, tagged with the instance it belongs to. Move-only. */
struct Context
{
    Context() = default;
    explicit Context (juce::AudioPluginInstance* p);   // message thread: reads the AU property
    Context (Context&& o) noexcept { swap (o); }
    Context& operator= (Context&& o) noexcept { Context t (std::move (o)); swap (t); return *this; }
    ~Context();
    void swap (Context& o) noexcept { std::swap (inst, o.inst); std::swap (observer, o.observer); std::swap (told, o.told); }

    /** Audio thread, before rendering `p`: tells the plugin when the workgroup changed. */
    void notify (const juce::AudioPluginInstance& p, void* workgroup) noexcept;

    const juce::AudioPluginInstance* inst = nullptr;
    void* observer = nullptr;   // retained AURenderContextObserver block
    void* told = nullptr;
};

/** Current device workgroup (set when the device starts), read by the racks. */
inline std::atomic<void*> current { nullptr };
inline std::atomic<bool> enabled { true };
}
