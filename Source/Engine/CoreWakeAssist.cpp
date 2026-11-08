#include "CoreWakeAssist.h"
#include <pthread/qos.h>

#if defined (__x86_64__) || defined (__i386__)
 #include <immintrin.h>
#endif

namespace stagerack
{

CoreWakeAssist::CoreWakeAssist (int numSpinners)
{
    for (int i = 0; i < numSpinners; ++i)
        threads.emplace_back ([this, i] { run (i); });
}

CoreWakeAssist::~CoreWakeAssist()
{
    quit = true;
    signal.fetch_add (1);
    signal.notify_all();

    for (auto& t : threads)
        t.join();
}

void CoreWakeAssist::armUntil (juce::int64 untilTicks) noexcept
{
    auto prev = spinUntil.load (std::memory_order_relaxed);

    while (untilTicks > prev && ! spinUntil.compare_exchange_weak (prev, untilTicks, std::memory_order_relaxed)) {}

    if (untilTicks > prev)
    {
        signal.fetch_add (1, std::memory_order_release);
        signal.notify_all();
    }
}

void CoreWakeAssist::setAlwaysOn (bool on) noexcept
{
    alwaysOn = on;
    signal.fetch_add (1, std::memory_order_release);
    signal.notify_all();
}

void CoreWakeAssist::run (int index)
{
    pthread_setname_np ("StageRack core wake assist");

    // Below normal threads: any plugin helper waking up pre-empts this immediately.
    pthread_set_qos_class_self_np (QOS_CLASS_DEFAULT, -15);

    uint32_t seen = signal.load (std::memory_order_acquire);

    while (! quit.load (std::memory_order_relaxed))
    {
        int n = 0;

        while (index < activeSpinners.load (std::memory_order_relaxed)
               && (alwaysOn.load (std::memory_order_relaxed)
                   || juce::Time::getHighResolutionTicks() < spinUntil.load (std::memory_order_relaxed)))
        {
           #if defined (__aarch64__) || defined (__arm__)
            __builtin_arm_yield();
           #else
            _mm_pause();
           #endif

            if ((++n & 1023) == 0 && quit.load (std::memory_order_relaxed))
                return;
        }

        signal.wait (seen, std::memory_order_acquire);
        seen = signal.load (std::memory_order_acquire);
    }
}

} // namespace stagerack
