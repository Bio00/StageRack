#include "WorkerPool.h"

#if JUCE_MAC
 #include <sys/sysctl.h>
#endif

#if defined (__x86_64__) || defined (__i386__)
 #include <immintrin.h>
#endif

namespace stagerack
{

static inline void cpuRelax() noexcept
{
   #if defined (__aarch64__) || defined (__arm__)
    __builtin_arm_yield();
   #elif defined (__x86_64__) || defined (__i386__)
    _mm_pause();
   #endif
}

// Short spin after each batch before going to sleep: keeps wake-up latency
// tiny when callbacks are back-to-back, but costs nothing while idle.
static constexpr int workerSpinIterations = 3000;

//==============================================================================
class WorkerPool::Worker final : public juce::Thread
{
public:
    Worker (WorkerPool& p, int threadIndexToUse)
        : juce::Thread ("StageRack DSP " + juce::String (threadIndexToUse)),
          pool (p), threadIndex (threadIndexToUse) {}

    void wake() noexcept
    {
        signal.fetch_add (1, std::memory_order_release);
        signal.notify_one();
    }

    void requestQuit()
    {
        quit.store (true);
        wake();
    }

    void run() override
    {
        juce::WorkgroupToken token;
        int joinedVersion = -1;
        uint32_t seen = signal.load (std::memory_order_acquire);

        for (;;)
        {
            uint32_t s;
            int spins = 0;

            while ((s = signal.load (std::memory_order_acquire)) == seen)
            {
                if (++spins < workerSpinIterations)
                    cpuRelax();
                else
                    signal.wait (seen, std::memory_order_acquire);
            }

            seen = s;

            if (quit.load (std::memory_order_acquire))
                break;

            if (const auto v = pool.workgroupVersion.load (std::memory_order_acquire); v != joinedVersion)
            {
                juce::AudioWorkgroup wg;
                {
                    const juce::SpinLock::ScopedLockType sl (pool.workgroupLock);
                    wg = pool.workgroup;
                }

                if (wg)
                    wg.join (token);
                else
                    token.reset();

                joinedVersion = v;
            }

            const auto gen = (uint32_t) (pool.state.load (std::memory_order_acquire) >> 32);
            pool.grabAndRun (gen, threadIndex);
        }

        token.reset();
    }

private:
    WorkerPool& pool;
    const int threadIndex;
    std::atomic<uint32_t> signal { 0 };
    std::atomic<bool> quit { false };
};

//==============================================================================
WorkerPool::WorkerPool()
{
    for (auto& b : busyMs)
        b.store (0.0);
}

WorkerPool::~WorkerPool()
{
    stop();
}

bool WorkerPool::hasPerformanceLevels()
{
   #if JUCE_MAC
    int levels = 0;
    size_t size = sizeof (levels);

    if (sysctlbyname ("hw.nperflevels", &levels, &size, nullptr, 0) == 0)
        return levels > 1;
   #endif

    return false;
}

int WorkerPool::getRecommendedWorkerCount()
{
    int perfCores = 0;

   #if JUCE_MAC
    int value = 0;
    size_t size = sizeof (value);

    if (sysctlbyname ("hw.perflevel0.physicalcpu", &value, &size, nullptr, 0) == 0)
        perfCores = value;
   #endif

    // Intel Macs have no hw.perflevel* keys: all physical cores are equal
    // (hyper-threads are not counted, they would share a core with the IO thread).
    if (perfCores <= 0)
        perfCores = juce::SystemStats::getNumPhysicalCpus();

    // One P-core is taken by the CoreAudio IO thread itself, which also does work.
    return juce::jlimit (0, maxThreads - 1, perfCores - 1);
}

juce::Thread::RealtimeOptions WorkerPool::realtimeOptionsFor (int blockSize, double sampleRate)
{
    const auto periodMs = 1000.0 * juce::jmax (16, blockSize) / (sampleRate > 0 ? sampleRate : 48000.0);

    return juce::Thread::RealtimeOptions{}
               .withPriority (10)
               .withPeriodMs (periodMs)
               .withProcessingTimeMs (periodMs * 0.6)
               .withMaximumProcessingTimeMs (periodMs);
}

void WorkerPool::start (int numWorkers, int blockSize, double sampleRate)
{
    stop();

    numWorkers = juce::jlimit (0, maxThreads - 1, numWorkers);

    const auto options = realtimeOptionsFor (blockSize, sampleRate);

    for (int i = 0; i < numWorkers; ++i)
    {
        auto w = std::make_unique<Worker> (*this, i + 1);

        if (! w->startRealtimeThread (options))
            w->startThread (juce::Thread::Priority::highest);

        workers.push_back (std::move (w));
    }
}

void WorkerPool::stop()
{
    for (auto& w : workers)
        w->requestQuit();

    for (auto& w : workers)
        w->stopThread (2000);

    workers.clear();
}

void WorkerPool::setWorkgroup (const juce::AudioWorkgroup& wg)
{
    {
        const juce::SpinLock::ScopedLockType sl (workgroupLock);
        workgroup = wg;
    }

    workgroupVersion.fetch_add (1, std::memory_order_release);
}

bool WorkerPool::grabAndRun (uint32_t gen, int threadIndex) noexcept
{
    const auto fn  = taskFn.load (std::memory_order_acquire);
    const auto ctx = taskContext.load (std::memory_order_acquire);
    const auto startTicks = juce::Time::getHighResolutionTicks();
    bool didWork = false;

    for (;;)
    {
        auto s = state.load (std::memory_order_acquire);
        int index = -1;

        for (;;)
        {
            const auto sGen  = (uint32_t) (s >> 32);
            const auto total = (uint32_t) ((s >> 16) & 0xffff);
            const auto next  = (uint32_t) (s & 0xffff);

            if (sGen != gen || next >= total)
                break;

            if (state.compare_exchange_weak (s, packState (gen, total, next + 1),
                                             std::memory_order_acq_rel, std::memory_order_acquire))
            {
                index = (int) next;
                break;
            }
        }

        if (index < 0)
            break;

        fn (ctx, index, threadIndex);
        didWork = true;

        remaining.fetch_sub (1, std::memory_order_acq_rel);
    }

    if (didWork && threadIndex < maxThreads)
        busyMs[(size_t) threadIndex].store (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - startTicks) * 1000.0,
                                            std::memory_order_relaxed);

    return didWork;
}

void WorkerPool::run (int numTasks, TaskFn fn, void* context, bool allowParallel) noexcept
{
    if (numTasks <= 0)
        return;

    for (size_t i = 0; i < (size_t) getNumThreads() && i < busyMs.size(); ++i)
        busyMs[i].store (0.0, std::memory_order_relaxed);

    if (workers.empty() || numTasks == 1 || ! allowParallel)
    {
        const auto t0 = juce::Time::getHighResolutionTicks();

        for (int i = 0; i < numTasks; ++i)
            fn (context, i, 0);

        busyMs[0].store (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0,
                         std::memory_order_relaxed);
        return;
    }

    numTasks = juce::jmin (numTasks, 0xffff);

    taskFn.store (fn, std::memory_order_relaxed);
    taskContext.store (context, std::memory_order_relaxed);
    remaining.store (numTasks, std::memory_order_relaxed);

    ++generation;
    state.store (packState (generation, (uint32_t) numTasks, 0), std::memory_order_release);

    // Wake only as many workers as there are tasks left for them.
    const auto toWake = juce::jmin ((int) workers.size(), numTasks - 1);

    for (int i = 0; i < toWake; ++i)
        workers[(size_t) i]->wake();

    grabAndRun (generation, 0);

    while (remaining.load (std::memory_order_acquire) > 0)
        cpuRelax();
}

double WorkerPool::getThreadBusyMs (int threadIndex) const noexcept
{
    if (! juce::isPositiveAndBelow (threadIndex, maxThreads))
        return 0.0;

    return busyMs[(size_t) threadIndex].load (std::memory_order_relaxed);
}

} // namespace stagerack
