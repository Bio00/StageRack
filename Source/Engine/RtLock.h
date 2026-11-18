#pragma once

#include <JuceHeader.h>
#include <atomic>

namespace stagerack::rt
{

inline void cpuPause() noexcept
{
   #if defined (__aarch64__) || defined (__arm64__)
    __builtin_arm_yield();
   #elif defined (__x86_64__) || defined (__i386__)
    __builtin_ia32_pause();
   #endif
}

/** How often the audio thread found a lock taken by the message thread (diagnostics). */
inline std::atomic<juce::uint32> lockWaits { 0 };    // waited, then got it
inline std::atomic<juce::uint32> lockMisses { 0 };   // gave up: that block (or rack) was silent

/*  The message thread holds the audio locks only to swap a few pointers (a new graph, a new
    plugin in a slot): well under a microsecond. Giving up at the first failed try turned that
    rare collision into a silent block (a click); waiting a bounded time costs nothing in the
    normal case and still never blocks the audio thread for long if the holder was preempted. */
inline bool enterWithin (juce::SpinLock& lock, double maxMicros) noexcept
{
    if (lock.tryEnter())
        return true;

    static const auto ticksPerMicro = (double) juce::Time::getHighResolutionTicksPerSecond() * 1.0e-6;
    const auto limit = (juce::int64) (maxMicros * ticksPerMicro);
    const auto start = juce::Time::getHighResolutionTicks();

    do
    {
        for (int i = 0; i < 16; ++i)
            cpuPause();

        if (lock.tryEnter())
        {
            lockWaits.fetch_add (1, std::memory_order_relaxed);
            return true;
        }
    }
    while (juce::Time::getHighResolutionTicks() - start < limit);

    lockMisses.fetch_add (1, std::memory_order_relaxed);
    return false;
}

/** Releases a lock taken with enterWithin (if it was taken). */
struct ScopedExit
{
    juce::SpinLock* lock = nullptr;
    ~ScopedExit() { if (lock != nullptr) lock->exit(); }
};

} // namespace stagerack::rt
