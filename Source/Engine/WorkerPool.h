#pragma once

#include <JuceHeader.h>
#include <atomic>

namespace stagerack
{

/**
    Realtime fork/join pool used by the audio callback to process racks in parallel.

    - Workers are realtime threads (time-constraint policy on macOS) and join the
      CoreAudio device workgroup, so the scheduler keeps them on the P-cores
      together with the device IO thread.
    - The audio thread takes part in the work itself; a batch is published with a
      single 64-bit atomic word (generation | total | next index), so there are no
      locks and no allocations on the realtime path.
    - Workers spin for a few microseconds after each batch, then sleep on a futex
      (std::atomic::wait), so an idle engine costs ~0% CPU.
*/
class WorkerPool
{
public:
    using TaskFn = void (*) (void* context, int taskIndex, int threadIndex);

    WorkerPool();
    ~WorkerPool();

    /** (Re)starts the worker threads. Call from a non-realtime thread. */
    void start (int numWorkers, int blockSize, double sampleRate);
    void stop();

    /** Sets the device workgroup; workers (re)join it on their next wake-up. */
    void setWorkgroup (const juce::AudioWorkgroup&);

    /** Joins the calling thread (the audio IO thread) to nothing: it already belongs to
        the device workgroup. Runs `numTasks` tasks across workers + caller and returns when
        all are done. Realtime safe. */
    void run (int numTasks, TaskFn fn, void* context, bool allowParallel = true) noexcept;

    int getNumWorkers() const noexcept       { return (int) workers.size(); }
    int getNumThreads() const noexcept       { return getNumWorkers() + 1; }

    /** Busy time in the last cycle for thread index (0 = audio thread). */
    double getThreadBusyMs (int threadIndex) const noexcept;

    static int getRecommendedWorkerCount();

    /** True on Apple Silicon (P-cores + E-cores); false on Intel Macs (all cores equal). */
    static bool hasPerformanceLevels();

    /** Time-constraint policy for a thread woken once per `blockSize` samples
        (period = block, computation = 60%, constraint = 100%), like CoreAudio's IO thread. */
    static juce::Thread::RealtimeOptions realtimeOptionsFor (int blockSize, double sampleRate);

    static constexpr int maxThreads = 32;

private:
    class Worker;

    bool grabAndRun (uint32_t generation, int threadIndex) noexcept;

    static constexpr uint64_t packState (uint32_t gen, uint32_t total, uint32_t next) noexcept
    {
        return ((uint64_t) gen << 32) | ((uint64_t) (total & 0xffff) << 16) | (uint64_t) (next & 0xffff);
    }

    std::atomic<uint64_t> state { 0 };
    std::atomic<int> remaining { 0 };
    std::atomic<TaskFn> taskFn { nullptr };
    std::atomic<void*> taskContext { nullptr };
    uint32_t generation = 0;

    std::vector<std::unique_ptr<Worker>> workers;

    juce::SpinLock workgroupLock;
    juce::AudioWorkgroup workgroup;
    std::atomic<int> workgroupVersion { 0 };

    std::array<std::atomic<double>, maxThreads> busyMs {};

    JUCE_DECLARE_NON_COPYABLE (WorkerPool)
};

} // namespace stagerack
