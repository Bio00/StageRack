#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cstdint>
#include <semaphore.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <os/os_sync_wait_on_address.h>
#include <os/clock.h>

namespace stagerack::sandbox
{

/*  EXPERIMENTAL plugin isolation: a plugin runs in a child process (this same executable with
    commandLineId), so a crash takes down that process only.
    - Control (load, prepare, state, editor window): JUCE ChildProcessCoordinator/Worker pipes,
      messages = ValueTree "Msg" with a request id; replies carry the same id.
    - Audio: one shared memory block per instance. The rack's audio thread writes the input,
      bumps `request` and posts a named semaphore; the child's realtime thread processes and
      stores `done`. The rack spins for it within the same callback: ZERO added latency. If the
      child is late or gone, the rack passes the input through (dry) instead of going silent. */

inline constexpr const char* commandLineId = "srk-sandbox";

struct Shared
{
    static constexpr std::uint32_t magicValue = 0x53524b31;   // "SRK1"
    static constexpr int maxChannels = 16, maxBlock = 4096, maxParams = 2048, ringSize = 1024, textSize = 24;

    std::uint32_t magic = magicValue;

    std::atomic<std::uint32_t> request { 0 }, done { 0 };   // block sequence numbers
    std::atomic<std::int32_t> numSamples { 0 }, bypassed { 0 }, numInputs { 0 }, numOutputs { 0 };
    std::atomic<std::int32_t> latency { 0 };
    std::atomic<std::int32_t> quit { 0 };
    std::atomic<float> processMs { 0.0f };                  // child: last processBlock time
    std::atomic<std::uint32_t> hostSleeping { 0 };          // host waits on `done` with os_sync (child must wake it)

    // play head of the block (written before `request`, read after it)
    double bpm = 120.0, ppq = 0.0, timeSec = 0.0;
    std::int32_t hasPosition = 0, playing = 0, timeSigNum = 4, timeSigDen = 4;

    // parameter changes host -> child (single producer, single consumer)
    struct Change { std::int32_t index; float value; };
    std::atomic<std::uint32_t> ringWrite { 0 }, ringRead { 0 };
    Change ring[ringSize];

    // current values and texts child -> host (refreshed by the child's message thread)
    std::atomic<float> values[maxParams];
    std::atomic<std::uint32_t> textVersion[maxParams];
    char texts[maxParams][textSize];

    float audioIn[maxChannels][maxBlock];
    float audioOut[maxChannels][maxBlock];

    bool pushChange (int index, float value) noexcept
    {
        const auto w = ringWrite.load (std::memory_order_relaxed);
        if (w - ringRead.load (std::memory_order_acquire) >= (std::uint32_t) ringSize)
            return false;
        ring[w % ringSize] = { index, value };
        ringWrite.store (w + 1, std::memory_order_release);
        return true;
    }

    template <typename Fn>
    void popChanges (Fn&& fn) noexcept
    {
        auto r = ringRead.load (std::memory_order_relaxed);
        const auto w = ringWrite.load (std::memory_order_acquire);
        for (; r != w; ++r)
            fn (ring[r % ringSize].index, ring[r % ringSize].value);
        ringRead.store (r, std::memory_order_release);
    }
};

/** A named POSIX shared memory block + the "request" semaphore. Names are short (macOS limit 31). */
class Channel
{
public:
    /** Host side: creates both. */
    static std::unique_ptr<Channel> create (const juce::String& baseName)
    {
        auto c = std::unique_ptr<Channel> (new Channel());
        c->name = baseName;
        c->owner = true;
        shm_unlink (c->shmName().toRawUTF8());
        sem_unlink (c->semName().toRawUTF8());

        const int fd = shm_open (c->shmName().toRawUTF8(), O_CREAT | O_RDWR | O_EXCL, 0600);
        if (fd < 0) return {};
        if (ftruncate (fd, (off_t) sizeof (Shared)) != 0) { close (fd); return {}; }
        void* mem = mmap (nullptr, sizeof (Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close (fd);
        if (mem == MAP_FAILED) return {};
        c->shared = new (mem) Shared();
        std::memset ((void*) c->shared->audioIn, 0, sizeof (Shared::audioIn) + sizeof (Shared::audioOut));   // pages touched now, not in the callback

        c->sem = sem_open (c->semName().toRawUTF8(), O_CREAT | O_EXCL, 0600, 0);
        if (c->sem == SEM_FAILED) { c->sem = nullptr; return {}; }
        return c;
    }

    /** Child side: opens what the host created. */
    static std::unique_ptr<Channel> open (const juce::String& baseName)
    {
        auto c = std::unique_ptr<Channel> (new Channel());
        c->name = baseName;
        const int fd = shm_open (c->shmName().toRawUTF8(), O_RDWR, 0600);
        if (fd < 0) return {};
        void* mem = mmap (nullptr, sizeof (Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close (fd);
        if (mem == MAP_FAILED) return {};
        c->shared = static_cast<Shared*> (mem);
        if (c->shared->magic != Shared::magicValue) return {};
        c->sem = sem_open (c->semName().toRawUTF8(), 0);
        if (c->sem == SEM_FAILED) { c->sem = nullptr; return {}; }
        return c;
    }

    ~Channel()
    {
        if (sem != nullptr) sem_close (sem);
        if (shared != nullptr) munmap ((void*) shared, sizeof (Shared));
        if (owner)
        {
            shm_unlink (shmName().toRawUTF8());
            sem_unlink (semName().toRawUTF8());
        }
    }

    Shared& get() noexcept           { return *shared; }
    void signal() noexcept           { sem_post (sem); }
    void wait() noexcept             { while (sem_wait (sem) != 0 && errno == EINTR) {} }
    const juce::String& getName() const noexcept { return name; }

private:
    Channel() = default;
    juce::String shmName() const     { return "/" + name; }
    juce::String semName() const     { return "/" + name + "s"; }

    juce::String name;
    Shared* shared = nullptr;
    sem_t* sem = nullptr;
    bool owner = false;
};

/** Control messages: a ValueTree with type, id and properties (+ "data" MemoryBlock for state). */
/** macOS 14.4+: a wait on a shared address that sleeps instead of spinning (futex-like). */
inline bool canSleepOnAddress() noexcept
{
    if (__builtin_available (macOS 14.4, *))
        return true;
    return false;
}

/** Host: sleeps until `done` changes from `seen` or timeoutNs passes. */
inline void sleepWhileDoneIs (Shared& s, std::uint32_t seen, std::uint64_t timeoutNs) noexcept
{
    if (__builtin_available (macOS 14.4, *))
        os_sync_wait_on_address_with_timeout ((void*) &s.done, seen, 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                              OS_CLOCK_MACH_ABSOLUTE_TIME, timeoutNs);
}

/** Child: after storing `done`, wakes the host if it went to sleep. */
inline void wakeHost (Shared& s) noexcept
{
    if (s.hostSleeping.load() != 0)
        if (__builtin_available (macOS 14.4, *))
            os_sync_wake_by_address_any ((void*) &s.done, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

inline juce::MemoryBlock pack (const juce::ValueTree& t)
{
    juce::MemoryOutputStream out;
    t.writeToStream (out);
    return out.getMemoryBlock();
}

inline juce::ValueTree unpack (const juce::MemoryBlock& mb)
{
    return juce::ValueTree::readFromData (mb.getData(), mb.getSize());
}

} // namespace stagerack::sandbox
