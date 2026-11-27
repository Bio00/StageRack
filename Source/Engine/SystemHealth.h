#pragma once

#include <JuceHeader.h>

namespace stagerack::systemhealth
{

/** Tells macOS this is live, latency-critical work for as long as the app runs: no App Nap
    (timers of a hidden window are not throttled), no timer coalescing, no idle system sleep
    (the display may still sleep). Call once at startup, message thread. */
void beginLiveActivity();

/** Low Power Mode (battery saver): CPU clocks are capped, plugin hops take much longer. */
bool isLowPowerMode();

/** 0 nominal, 1 fair, 2 serious, 3 critical (NSProcessInfoThermalState): from 2 the Mac slows down. */
int thermalState();

/** 0 normal, 1 warning, 2 critical: the system is short of memory (pages of the audio code may be
    compressed or swapped out, which stalls the audio thread). Starts listening on first call. */
int memoryPressure();

/** Microphone (audio input) access for this app: 0 not asked yet, 1 restricted, 2 denied,
    3 granted (AVAuthorizationStatus). Without it CoreAudio delivers silent inputs, with no error.
    macOS ties the permission to the code signature: every ad-hoc build is a new app for it. */
int micAccess();

/** Asks for microphone access if it was never asked (shows the system prompt), message thread. */
void requestMicAccess();

/** Short text of what is wrong right now ("LOW POWER MODE", "HOT", "LOW MEMORY", "NO MIC ACCESS"), empty if all is well. */
juce::String problems();

} // namespace stagerack::systemhealth
