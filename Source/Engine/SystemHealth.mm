#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#include <dispatch/dispatch.h>
#include <atomic>
#include "SystemHealth.h"

namespace stagerack::systemhealth
{

void beginLiveActivity()
{
    static id<NSObject> activity = nil;

    if (activity != nil)
        return;

    // UserInitiated includes IdleSystemSleepDisabled; LatencyCritical keeps timers precise
    activity = [[NSProcessInfo processInfo] beginActivityWithOptions: NSActivityUserInitiated | NSActivityLatencyCritical
                                                              reason: @"Live audio processing"];
   #if ! __has_feature(objc_arc)
    [activity retain];
   #endif
}

bool isLowPowerMode()
{
    auto* info = [NSProcessInfo processInfo];

    if ([info respondsToSelector: @selector (isLowPowerModeEnabled)])
        return [info isLowPowerModeEnabled];

    return false;
}

int thermalState()
{
    return (int) [[NSProcessInfo processInfo] thermalState];
}

static std::atomic<int> pressure { 0 };

int memoryPressure()
{
    static dispatch_source_t source = nullptr;

    if (source == nullptr)
    {
        source = dispatch_source_create (DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                                         DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
                                         dispatch_get_main_queue());
        if (source != nullptr)
        {
            dispatch_source_set_event_handler (source, ^{
                const auto flags = dispatch_source_get_data (source);
                pressure = (flags & DISPATCH_MEMORYPRESSURE_CRITICAL) ? 2 : ((flags & DISPATCH_MEMORYPRESSURE_WARN) ? 1 : 0);
            });
            dispatch_resume (source);
        }
    }

    return pressure.load();
}

static std::atomic<int> micStatus { -1 };
static std::atomic<double> micCheckedAt { -1.0e9 };

int micAccess()
{
    // the answer changes only when the user flips it in System Settings: ask at most every 3 s
    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (micStatus.load() < 0 || now - micCheckedAt.load() > 3000.0)
    {
        micStatus = (int) [AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeAudio];
        micCheckedAt = now;
    }

    return micStatus.load();
}

void requestMicAccess()
{
    if (micAccess() != (int) AVAuthorizationStatusNotDetermined)
        return;

    [AVCaptureDevice requestAccessForMediaType: AVMediaTypeAudio completionHandler: ^(BOOL) { micStatus = -1; }];
}

juce::String problems()
{
    juce::StringArray p;

    if (const auto mic = micAccess(); mic == (int) AVAuthorizationStatusDenied || mic == (int) AVAuthorizationStatusRestricted)
        p.add ("NO MIC ACCESS");

    if (isLowPowerMode())       p.add ("LOW POWER MODE");
    if (thermalState() >= 2)    p.add (thermalState() >= 3 ? "OVERHEATING" : "HOT");
    if (memoryPressure() >= 1)  p.add (memoryPressure() >= 2 ? "OUT OF MEMORY" : "LOW MEMORY");

    return p.joinIntoString (" \xc2\xb7 ");
}

} // namespace stagerack::systemhealth
