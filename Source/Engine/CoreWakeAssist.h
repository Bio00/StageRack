#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <thread>

namespace stagerack
{

/**
    Keeps P-cores awake around the callbacks where a heavy plugin hop is due.

    Why: plugins like De-Feedback wake a private helper thread at each hop and make the
    audio thread wait for it. When few instances run, the P-cores are in deep idle and
    waking one takes ~0.2-0.3 ms, half of a 16-sample buffer: measured, the audio thread
    then spends ~58% of the hop just waiting. With many instances some cores are always
    awake, which is why 8 instances ran better than 1.

    How: a few spinner threads at a priority BELOW normal threads (default QoS, -15). They
    keep cores out of deep idle, and a waking helper pre-empts them instantly on an already
    running core. The audio callback arms them one or two callbacks before each scheduled
    hop (known from HopAligner), so they only spin around hops instead of all the time.
*/
class CoreWakeAssist
{
public:
    explicit CoreWakeAssist (int numSpinners = 2);
    ~CoreWakeAssist();

    /** Realtime safe: spin until `untilTicks` (Time::getHighResolutionTicks) at least. */
    void armUntil (juce::int64 untilTicks) noexcept;

    /** How many of the spinner threads take part (the others stay asleep). */
    void setActiveSpinners (int n) noexcept         { activeSpinners = n; }

    /** Spin permanently (true) or only when armed (false). */
    void setAlwaysOn (bool) noexcept;

private:
    void run (int index);

    std::vector<std::thread> threads;
    std::atomic<juce::int64> spinUntil { 0 };
    std::atomic<uint32_t> signal { 0 };
    std::atomic<bool> alwaysOn { false }, quit { false };
    std::atomic<int> activeSpinners { 1 };
};

} // namespace stagerack
