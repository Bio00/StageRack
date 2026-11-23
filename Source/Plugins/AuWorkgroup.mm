#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include <os/workgroup.h>
#include "AuWorkgroup.h"

namespace stagerack::auwg
{
void* copyDeviceWorkgroup (const juce::String& deviceName)
{
    AudioObjectPropertyAddress addr { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr || size == 0)
        return nullptr;

    std::vector<AudioDeviceID> ids (size / sizeof (AudioDeviceID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr)
        return nullptr;

    for (auto id : ids)
    {
        CFStringRef name = nullptr;
        UInt32 ns = sizeof (name);
        AudioObjectPropertyAddress na { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData (id, &na, 0, nullptr, &ns, &name) != noErr || name == nullptr)
            continue;

        const auto match = juce::String::fromCFString (name) == deviceName;
        CFRelease (name);
        if (! match)
            continue;

        os_workgroup_t wg = nullptr;
        UInt32 ws = sizeof (wg);
        AudioObjectPropertyAddress wa { kAudioDevicePropertyIOThreadOSWorkgroup, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData (id, &wa, 0, nullptr, &ws, &wg) == noErr && wg != nullptr)
            return (__bridge void*) wg;   // the property returns +1
    }
    return nullptr;
}

void releaseWorkgroup (void* wg)
{
    if (wg != nullptr)
        CFRelease (wg);
}

Context::Context (juce::AudioPluginInstance* p)
{
    if (p == nullptr || p->getPluginDescription().pluginFormatName != "AudioUnit")
        return;

    auto* client = p->getAudioUnitClient();
    auto unit = client != nullptr ? (AudioUnit) client->getAudioUnitHandle() : nullptr;
    if (unit == nullptr)
        return;

    AURenderContextObserver obs = nil;
    UInt32 size = sizeof (obs);
    if (AudioUnitGetProperty (unit, kAudioUnitProperty_RenderContextObserver, kAudioUnitScope_Global, 0, &obs, &size) == noErr && obs != nil)
    {
        observer = (__bridge void*) [obs copy];
        inst = p;
    }
}

Context::~Context()
{
    if (observer != nullptr)
        [(id) observer release];
}

void Context::notify (const juce::AudioPluginInstance& p, void* workgroup) noexcept
{
    if (observer == nullptr || &p != inst || workgroup == told)
        return;

    told = workgroup;
    AudioUnitRenderContext ctx {};
    ctx.workgroup = (__bridge os_workgroup_t) workgroup;
    ((__bridge AURenderContextObserver) observer) (&ctx);
}
}
