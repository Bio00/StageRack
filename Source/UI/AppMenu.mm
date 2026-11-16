#import <Cocoa/Cocoa.h>
#include "AppMenu.h"

namespace stagerack
{

bool currentEventIsKeyDown()
{
    auto* ev = [NSApp currentEvent];
    return ev != nil && ev.type == NSEventTypeKeyDown;
}

} // namespace stagerack
