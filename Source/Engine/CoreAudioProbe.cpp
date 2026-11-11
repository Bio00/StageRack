#include "CoreAudioProbe.h"

#include <CoreAudio/CoreAudio.h>
#include <mach-o/dyld.h>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace stagerack::coreaudio
{

namespace
{
    // never destroyed: the waiting thread may still be blocked in waitpid at exit
    struct State
    {
        std::once_flag started;
        std::atomic<bool> ready { false };
        std::atomic<pid_t> child { 0 };
        std::atomic<bool> stopping { false };
        std::mutex lock;
        std::condition_variable cv;
    };

    State& state()
    {
        static auto* s = new State();
        return *s;
    }

    void markReady (State& s)
    {
        {
            std::lock_guard<std::mutex> g (s.lock);
            s.ready = true;
        }
        s.cv.notify_all();
    }

    pid_t spawnChild()
    {
        char path[PATH_MAX];
        uint32_t size = sizeof (path);

        if (_NSGetExecutablePath (path, &size) != 0)
            return 0;

        char* argv[] = { path, const_cast<char*> (probeArgument), nullptr };
        pid_t pid = 0;
        return posix_spawn (&pid, path, nullptr, nullptr, argv, environ) == 0 ? pid : 0;
    }
}

int runProbeChild()
{
    // test hook: behave like a stuck coreaudiod (the app must still open and show it)
    // (value = seconds before answering, 0 = never)
    if (const char* hang = std::getenv ("STAGERACK_TEST_COREAUDIO_HANG"))
    {
        if (const int seconds = std::atoi (hang); seconds > 0)
            sleep ((unsigned) seconds);
        else
            for (;;) pause();
    }

    const AudioObjectPropertyAddress addr { kAudioHardwarePropertyDevices,
                                            kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    return AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &addr, 0, nullptr, &size) == noErr ? 0 : 1;
}

void startProbe()
{
    auto& s = state();
    std::call_once (s.started, [&s]
    {
        std::thread ([&s]
        {
            for (int attempt = 0; ! s.stopping; ++attempt)
            {
                const auto pid = spawnChild();

                if (pid <= 0)
                    break;   // cannot probe: fall back to opening CoreAudio directly

                s.child = pid;
                int status = 0;
                while (waitpid (pid, &status, 0) < 0 && errno == EINTR) {}
                s.child = 0;

                // exited (even with an error code): coreaudiod answered
                if (WIFEXITED (status) || attempt >= 5)
                    break;

                // crashed or killed: try again
                std::this_thread::sleep_for (std::chrono::milliseconds (500));
            }

            if (! s.stopping)
                markReady (s);
        }).detach();
    });
}

bool isReady()
{
    return state().ready.load();
}

bool waitUntilReady (int timeoutMs)
{
    startProbe();
    auto& s = state();
    std::unique_lock<std::mutex> l (s.lock);
    return s.cv.wait_for (l, std::chrono::milliseconds (timeoutMs), [&s] { return s.ready.load(); });
}

void stopProbe()
{
    auto& s = state();
    s.stopping = true;

    if (const auto pid = s.child.load(); pid > 0)
        kill (pid, SIGKILL);
}

} // namespace stagerack::coreaudio
