#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

namespace stagerack
{

/** Counts the audio dropouts of the live engine, as they happen:
    - late: our callback took longer than the buffer period (the bench's "over budget");
    - silent: the rack list was being changed (message thread held the lock): one block of silence;
    - skipped: CoreAudio did not call us for one or more periods (gap in the host time of the
      buffers: the IO thread woke up late, the device or the system stalled). CoreAudio's own
      overload notification stays silent in some of these cases (measured with the Mac's
      speakers), the host time does not.
    The audio thread only writes atomics and a lock-free ring of events; the message thread
    reads the counters and drains the ring into a log (see DropoutLog). */
class DropoutMonitor
{
public:
    struct Event
    {
        juce::uint64 callback = 0;
        double hostSec = 0.0;      // host time of the buffer (mach seconds), 0 if unknown
        float ms = 0.0f;           // time our callback took
        float budgetMs = 0.0f;
        int skipped = 0;           // periods with no callback before this one
        bool silent = false;       // the racks were being edited (lock busy): this block was silence
        int heaviestRack = -1;     // id of the rack that took longest in this callback
        float heaviestMs = 0.0f;
    };

    /** Audio thread: a new device run starts (no gap to the previous one). */
    void reset() noexcept { lastHostNs = 0; }

    /** Audio thread, at the end of every callback. */
    void note (juce::uint64 callback, juce::uint64 hostNs, int numSamples, double sampleRate,
               double elapsedSec, int heaviestRack, float heaviestMs, bool silent = false) noexcept
    {
        const auto periodSec = numSamples / juce::jmax (1.0, sampleRate);
        int skipped = 0;

        if (hostNs != 0)
        {
            if (lastHostNs != 0 && hostNs > lastHostNs && lastSamples == numSamples)
            {
                const auto periods = (double) (hostNs - lastHostNs) * 1.0e-9 / periodSec;
                if (periods > 1.5)
                    skipped = (int) std::lround (periods) - 1;
            }

            lastHostNs = hostNs;
            lastSamples = numSamples;
        }

        const bool late = elapsedSec > periodSec;

        if (! late && skipped == 0 && ! silent)
            return;

        if (silent)  silentCount.fetch_add (1, std::memory_order_relaxed);
        if (late)    lateCount.fetch_add (1, std::memory_order_relaxed);
        if (skipped) skippedCount.fetch_add ((juce::uint32) skipped, std::memory_order_relaxed);
        lastDropCallback.store (callback, std::memory_order_relaxed);

        const auto w = writePos.load (std::memory_order_relaxed);

        if (w - readPos.load (std::memory_order_acquire) < ring.size())
        {
            auto& e = ring[w % ring.size()];
            e.callback = callback;
            e.hostSec = (double) hostNs * 1.0e-9;
            e.ms = (float) (elapsedSec * 1000.0);
            e.budgetMs = (float) (periodSec * 1000.0);
            e.skipped = skipped;
            e.heaviestRack = heaviestRack;
            e.heaviestMs = heaviestMs;
            e.silent = silent;
            writePos.store (w + 1, std::memory_order_release);
        }
        else
        {
            lostEvents.fetch_add (1, std::memory_order_relaxed);
        }
    }

    /** Message thread: takes the events logged since the last call. */
    template <typename Fn>
    void drain (Fn&& fn)
    {
        const auto w = writePos.load (std::memory_order_acquire);
        auto r = readPos.load (std::memory_order_relaxed);

        for (; r < w; ++r)
            fn (ring[r % ring.size()]);

        readPos.store (r, std::memory_order_release);
    }

    juce::uint32 getLate() const noexcept     { return lateCount.load (std::memory_order_relaxed); }
    juce::uint32 getSkipped() const noexcept  { return skippedCount.load (std::memory_order_relaxed); }
    juce::uint32 getSilent() const noexcept   { return silentCount.load (std::memory_order_relaxed); }
    juce::uint32 getTotal() const noexcept    { return getLate() + getSkipped() + getSilent(); }
    juce::uint64 getLastDropCallback() const noexcept { return lastDropCallback.load (std::memory_order_relaxed); }

    /** Message thread: the counters start again from 0 (the log keeps everything). */
    void clearCounters() noexcept
    {
        lateCount = 0;
        skippedCount = 0;
        silentCount = 0;
    }

private:
    // audio thread only
    juce::uint64 lastHostNs = 0;
    int lastSamples = 0;

    std::atomic<juce::uint32> lateCount { 0 }, skippedCount { 0 }, silentCount { 0 }, lostEvents { 0 };
    std::atomic<juce::uint64> lastDropCallback { 0 };

    std::array<Event, 256> ring {};
    std::atomic<juce::uint64> writePos { 0 }, readPos { 0 };
};

} // namespace stagerack
