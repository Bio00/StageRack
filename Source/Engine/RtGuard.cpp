#include "RtGuard.h"

#include <pthread.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <cxxabi.h>
#include <atomic>

extern "C"
{
    typedef void (malloc_logger_t) (uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result, uint32_t numHotFramesToSkip);
    extern malloc_logger_t* malloc_logger;
}

namespace stagerack::rt
{

namespace
{
    // Scope state per thread in a pthread key (not thread_local: Darwin's TLV may allocate on first
    // access, from inside the malloc hook). Value: bit 0 = current scope is host, bits 1.. = depth;
    // a small stack of the previous values is enough (scopes nest a few levels at most).
    pthread_key_t key;
    std::atomic<bool> installed { false };
    std::atomic<juce::uint32> hits { 0 };

    constexpr int maxStacks = 12, maxFrames = 24;
    void* stacks[maxStacks][maxFrames] {};
    int stackDepth[maxStacks] {};
    std::atomic<int> stacksTaken { 0 };

    struct ScopeStack { uintptr_t v[16]; int n = 0; };

    pthread_key_t stackKey;

    void hook (uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t)
    {
        const auto v = (uintptr_t) pthread_getspecific (key);
        if ((v & 1u) == 0)
            return;   // not in our audio code (or in a plugin call)

        // re-entrance: backtrace() must not count itself
        pthread_setspecific (key, (void*) (v & ~(uintptr_t) 1));
        hits.fetch_add (1, std::memory_order_relaxed);

        int slot = stacksTaken.load (std::memory_order_relaxed);
        if (slot < maxStacks && stacksTaken.compare_exchange_strong (slot, slot + 1))
            stackDepth[slot] = backtrace (stacks[slot], maxFrames);

        pthread_setspecific (key, (void*) v);
    }

    struct Init
    {
        Init()
        {
            pthread_key_create (&key, nullptr);
            pthread_key_create (&stackKey, nullptr);
        }
    };
    Init& init() { static Init i; return i; }
}

void enterScope (bool host) noexcept
{
    if (! installed.load (std::memory_order_relaxed))
        return;

    // previous value kept in a per-thread fixed stack (allocated once per thread, outside host scope)
    auto* st = (ScopeStack*) pthread_getspecific (stackKey);
    if (st == nullptr)
    {
        const auto saved = pthread_getspecific (key);
        pthread_setspecific (key, nullptr);   // this allocation is the guard's own
        st = new ScopeStack();
        pthread_setspecific (stackKey, st);
        pthread_setspecific (key, saved);
    }

    const auto v = (uintptr_t) pthread_getspecific (key);
    if (st->n < 16)
        st->v[st->n] = v;
    ++st->n;
    pthread_setspecific (key, (void*) (uintptr_t) (host ? 1u : 0u));
}

void leaveScope() noexcept
{
    if (! installed.load (std::memory_order_relaxed))
        return;

    auto* st = (ScopeStack*) pthread_getspecific (stackKey);
    if (st == nullptr || st->n == 0)
        return;

    --st->n;
    pthread_setspecific (key, st->n < 16 ? (void*) st->v[st->n] : nullptr);
}

void installGuard()
{
    init();
    if (! installed.exchange (true))
        malloc_logger = hook;
}

bool guardInstalled() noexcept       { return installed.load(); }
juce::uint32 guardHits() noexcept    { return hits.load(); }

void resetGuard() noexcept
{
    hits = 0;
    stacksTaken = 0;
}

juce::String guardReport()
{
    juce::String out;
    const int n = juce::jmin (stacksTaken.load(), maxStacks);
    juce::StringArray seen;

    for (int i = 0; i < n; ++i)
    {
        juce::String s;
        for (int f = 2; f < stackDepth[i]; ++f)   // skip the hook and libmalloc
        {
            Dl_info info {};
            if (dladdr (stacks[i][f], &info) == 0 || info.dli_sname == nullptr)
                continue;
            int status = 0;
            char* dem = abi::__cxa_demangle (info.dli_sname, nullptr, nullptr, &status);
            juce::String name (status == 0 && dem != nullptr ? dem : info.dli_sname);
            std::free (dem);
            if (name.contains ("malloc") || name.startsWith ("operator new") || name.contains ("_xzm_"))
                continue;
            s << "      " << name.substring (0, 160) << "\n";
            if (s.length() > 1400) break;
        }
        if (! seen.contains (s))
        {
            seen.add (s);
            out << "    allocation #" << (i + 1) << " in host audio code:\n" << s;
        }
    }
    return out;
}

} // namespace stagerack::rt
