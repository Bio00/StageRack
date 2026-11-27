#pragma once

#include <JuceHeader.h>

namespace stagerack
{

/** Tells when the plugin chains do not fit in the buffer, and which buffer would be enough.
    Message thread only: fed with the dropout events (DropoutLog::poll) and updated by the UI
    timer; the top bar shows it ("BUFFER TOO SMALL -> USE 32").

    Only dropouts where OUR processing took most of the period count: a device or system stall
    (CoreAudio called late while we were nearly idle) is not cured by a bigger buffer.

    The recommendation models a chain as a fixed part (a hop of De-Feedback / EverClean: the
    same work whatever the buffer) plus a part that grows with the buffer (continuous plugins,
    from the average load), and picks the smallest available buffer where the worst chain uses
    at most `headroom` of the period (the rest is for CoreAudio and the driver: Dante leaves less
    than the Mac's own outputs). Seen 2026-11-26: De-Feedback + soothe3 in series at 16 samples
    peaked at 0.35 ms of 0.333 ms; at 32 samples never over. */
class BudgetAdvisor
{
public:
    static constexpr double windowSec = 10.0;   // dropouts counted over this time ...
    static constexpr int minEvents = 3;         // ... at least this many to speak
    static constexpr double clearSec = 60.0;    // a clean minute clears the warning
    static constexpr float busyRatio = 0.75f;   // our work took at least this share of the period
    static constexpr float headroom = 0.7f;     // target: worst chain within 70% of the period

    /** A dropout: `ms` the whole callback, `heaviestMs` the heaviest rack (chain) in it. */
    void event (double nowSec, float ms, float heaviestMs, float budgetMs, bool silent, int rackId)
    {
        const auto worst = juce::jmax (ms, heaviestMs);

        if (silent || budgetMs <= 0.0f || worst < busyRatio * budgetMs)
            return;

        times.add (nowSec);
        lastEventSec = nowSec;

        if (worst > worstMs)
        {
            worstMs = worst;
            worstRack = rackId;
        }
    }

    /** Re-evaluates (call a few times a second). `available` = buffer sizes the device offers,
        `avgLoad` = average share of the period our callback takes. */
    void update (double nowSec, int bufferSize, double sampleRate, const juce::Array<int>& available, double avgLoad)
    {
        if (bufferSize != lastBuffer || std::abs (sampleRate - lastRate) > 1.0)
        {
            // a new buffer (often the one we suggested): judge it on its own
            reset();
            lastBuffer = bufferSize;
            lastRate = sampleRate;
            return;
        }

        while (! times.isEmpty() && times.getFirst() < nowSec - windowSec)
            times.remove (0);

        if (! active && times.size() >= minEvents)
            active = true;

        if (active && nowSec - lastEventSec > clearSec)
        {
            reset();
            return;
        }

        if (active)
            recommended = recommend (bufferSize, sampleRate, available, avgLoad);
    }

    /** The smallest buffer (from `available`, above `bufferSize`) where the worst chain fits;
        the largest available if none does, 0 if there is nothing bigger. */
    int recommend (int bufferSize, double sampleRate, const juce::Array<int>& available, double avgLoad) const
    {
        const auto period0 = 1000.0 * bufferSize / juce::jmax (1.0, sampleRate);
        const auto avgMs = juce::jlimit (0.0, (double) worstMs, avgLoad * period0);
        const auto fixedMs = juce::jmax (0.0, (double) worstMs - avgMs);

        auto sizes = available;
        sizes.sort();
        int biggest = 0;

        for (auto b : sizes)
        {
            if (b <= bufferSize)
                continue;

            biggest = b;
            const auto period = 1000.0 * b / juce::jmax (1.0, sampleRate);

            if (fixedMs + avgMs * b / juce::jmax (1, bufferSize) <= headroom * period)
                return b;
        }

        return biggest;
    }

    bool isActive() const noexcept         { return active; }
    int getRecommended() const noexcept    { return recommended; }
    float getWorstMs() const noexcept      { return worstMs; }
    int getWorstRack() const noexcept      { return worstRack; }
    int getBuffer() const noexcept         { return lastBuffer; }

    void reset()
    {
        times.clearQuick();
        active = false;
        recommended = 0;
        worstMs = 0.0f;
        worstRack = -1;
    }

private:
    juce::Array<double> times;
    double lastEventSec = -1.0e9;
    bool active = false;
    int recommended = 0;
    float worstMs = 0.0f;
    int worstRack = -1;
    int lastBuffer = 0;
    double lastRate = 0.0;
};

} // namespace stagerack
