#pragma once

#include <JuceHeader.h>

namespace stagerack
{

/**
    Zero-latency load spreading for frame-based plugins.

    Many heavy plugins (neural-network de-feedback/denoise, FFT processors) accumulate
    audio internally and do all the heavy work once per internal frame ("hop"): e.g.
    De-Feedback runs its network every 192 samples, costing ~0.3-0.6 ms in one callback
    and almost nothing in the others. With several instances started together their hops
    land in the SAME device callback, so one callback has to do all of them at once.

    The aligner measures each plugin's per-call cost to detect its hop, then feeds it a few
    blocks of silence (once, before it goes live) so its hop falls in the least busy callback
    phase. Instances end up spread evenly across the hop period. No latency is added: the
    plugin's output timing is unchanged, only its internal frame phase moves.
*/
class HopAligner
{
public:
    struct Result
    {
        int hopCalls = 0;   // hop length in device callbacks (0 = no periodic hop found)
        int phase = -1;     // callback phase (0..hopCalls-1) its hop now lands on
    };

    /** Detects the hop and shifts it to `targetPhase` (relative to callback index
        `liveCallbackIndex()`, the engine callback the plugin will first run in; it is
        asked after the measurement, just before the final priming).
        The plugin must already be prepared. Not realtime: call off the audio thread,
        while the plugin is not being processed. */
    static Result detectAndAlign (juce::AudioPluginInstance&, int blockSize,
                                  std::function<int (int hopCalls)> chooseTargetPhase,
                                  std::function<juce::uint64()> liveCallbackIndex);
};

} // namespace stagerack
