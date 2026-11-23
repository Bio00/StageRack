#import <Cocoa/Cocoa.h>
#include "AppMenu.h"

namespace stagerack
{

bool currentEventIsKeyDown()
{
    auto* ev = [NSApp currentEvent];
    return ev != nil && ev.type == NSEventTypeKeyDown;
}

void installKeyMonitor (std::function<bool (void*, int, bool, bool, bool)> handler)
{
    static std::function<bool (void*, int, bool, bool, bool)> current;
    static id monitor = nil;
    current = std::move (handler);

    if (monitor != nil)
        return;

    monitor = [NSEvent addLocalMonitorForEventsMatchingMask: NSEventMaskKeyDown
                                                    handler: ^NSEvent* (NSEvent* e)
    {
        if (! current || e.window == nil)
            return e;

        NSString* chars = e.charactersIgnoringModifiers;
        if (chars.length != 1)
            return e;

        int code = (int) [chars.lowercaseString characterAtIndex: 0];
        const bool mods = (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) != 0;
        const bool typing = [e.window.firstResponder isKindOfClass: [NSText class]];   // a native text field

        return current ((__bridge void*) e.window.contentView, code, e.isARepeat, mods, typing) ? nil : e;
    }];
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
