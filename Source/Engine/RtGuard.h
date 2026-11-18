#pragma once

#include <JuceHeader.h>

namespace stagerack::rt
{

/*  Allocation guard for the audio thread (a small RealtimeSanitizer of our own).
    Our audio code runs inside HostScope; calls into a plugin run inside PluginScope (what a
    plugin allocates is its business, we can't fix it). When the guard is installed, every
    malloc/free made while the innermost scope is ours is counted and the first stacks are kept.
    Uses macOS's malloc_logger hook: nothing is installed unless a test asks for it, and the
    scopes cost one pthread_getspecific / setspecific pair when the guard is off. */

void installGuard();                // tests / benches only
bool guardInstalled() noexcept;
juce::uint32 guardHits() noexcept;  // allocations seen in host scope
juce::String guardReport();         // symbolized stacks of the first hits (message thread)
void resetGuard() noexcept;

void enterScope (bool host) noexcept;
void leaveScope() noexcept;

/** Audio code of ours: allocations here are bugs. */
struct HostScope   { HostScope() noexcept   { enterScope (true); }  ~HostScope()   { leaveScope(); } };
/** A call into a plugin: its allocations are not ours. */
struct PluginScope { PluginScope() noexcept { enterScope (false); } ~PluginScope() { leaveScope(); } };

} // namespace stagerack::rt
