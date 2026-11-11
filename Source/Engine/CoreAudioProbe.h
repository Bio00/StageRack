#pragma once

namespace stagerack::coreaudio
{

/** The first CoreAudio call of a process connects to coreaudiod and waits for its reply with
    no timeout. If coreaudiod is stuck (seen with a freshly installed HAL driver spinning it at
    100% CPU) that call never returns and the app hangs before showing any window. Doing the
    call on a background thread is not enough: it holds the HAL's lock while it waits, and
    AppKit itself (the App Nap audio monitor) then asks CoreAudio on the main thread and blocks.

    So the probe runs in a child process (this executable with probeArgument) that makes one
    CoreAudio call and exits. Until it has answered, this process must not touch CoreAudio. */
inline constexpr const char* probeArgument = "--coreaudio-probe";

void startProbe();
bool isReady();

/** Starts the probe if needed and waits up to timeoutMs for it. */
bool waitUntilReady (int timeoutMs);

/** Kills a probe child still waiting for coreaudiod (at quit). */
void stopProbe();

/** Body of the child process: one CoreAudio call, returns the exit code. */
int runProbeChild();

} // namespace stagerack::coreaudio
