#import <Cocoa/Cocoa.h>
#include "AppMenu.h"

namespace stagerack
{

bool currentEventIsKeyDown()
{
    auto* ev = [NSApp currentEvent];
    return ev != nil && ev.type == NSEventTypeKeyDown;
}

void markAsBackgroundProcess()
{
    // Child processes (plugin scanner, latency probe) run this same bundle executable. Without
    // this they check in with LaunchServices as a normal app the moment AppKit starts: the Dock
    // shows a second StageRack icon and keeps it among the recent apps. LSBackgroundOnly read
    // from the in-memory Info.plist before NSApplication exists makes them background processes.
    // (Process::setDockIconVisible later is too late: the check-in has already happened.)
    @autoreleasepool
    {
        if (auto* info = (NSMutableDictionary*) [[NSBundle mainBundle] infoDictionary];
            [info isKindOfClass: [NSMutableDictionary class]])
            info[@"LSBackgroundOnly"] = @YES;
    }
}

void markAsAccessoryProcess()
{
    @autoreleasepool
    {
        if (auto* info = (NSMutableDictionary*) [[NSBundle mainBundle] infoDictionary];
            [info isKindOfClass: [NSMutableDictionary class]])
            info[@"LSUIElement"] = @YES;
    }
}

} // namespace stagerack
