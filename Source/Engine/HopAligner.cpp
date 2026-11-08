#include "HopAligner.h"
#include <map>

namespace stagerack
{

static void processCalls (juce::AudioPluginInstance& p, juce::AudioBuffer<float>& buf, int numCalls,
                          std::vector<double>* times)
{
    juce::MidiBuffer midi;

    for (int i = 0; i < numCalls; ++i)
    {
        buf.clear();
        midi.clear();
        const auto t0 = juce::Time::getHighResolutionTicks();

        {
            const juce::ScopedLock cl (p.getCallbackLock());
            p.processBlock (buf, midi);
        }

        if (times != nullptr)
            times->push_back (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0);
    }
}

HopAligner::Result HopAligner::detectAndAlign (juce::AudioPluginInstance& p, int blockSize,
                                               std::function<int (int)> chooseTargetPhase,
                                               std::function<juce::uint64()> liveCallbackIndex)
{
    if (blockSize <= 0)
        return {};

    const int channels = juce::jmax (2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buf (channels, blockSize);

    // ~250 ms of audio at most: enough to see 20+ hops of a few hundred samples.
    constexpr int warmupCalls = 16;
    const int calls = juce::jlimit (96, 768, 12288 / blockSize);
    std::vector<double> times;
    times.reserve ((size_t) calls);
    processCalls (p, buf, calls, &times);

    std::vector<double> settled (times.begin() + warmupCalls, times.end());
    auto sorted = settled;
    std::nth_element (sorted.begin(), sorted.begin() + (long) sorted.size() / 2, sorted.end());
    const auto median = sorted[sorted.size() / 2];
    const auto threshold = juce::jmax (median * 4.0, median + 0.03);

    std::vector<int> spikes;
    for (int i = 0; i < (int) settled.size(); ++i)
        if (settled[(size_t) i] > threshold)
            spikes.push_back (i + warmupCalls);

    if (spikes.size() < 4)
        return {};

    // the hop = the dominant distance between heavy calls
    std::map<int, int> gaps;
    for (size_t i = 1; i < spikes.size(); ++i)
        ++gaps[spikes[i] - spikes[i - 1]];

    auto best = std::max_element (gaps.begin(), gaps.end(), [] (auto& a, auto& b) { return a.second < b.second; });
    const int hopCalls = best->first;

    if (hopCalls < 2 || hopCalls > 128 || best->second < (int) (0.6 * (double) (spikes.size() - 1)))
        return {};

    // Next heavy call, counted from the next call the plugin will receive.
    const int last = spikes.back();
    const int next = last + hopCalls * ((calls - last + hopCalls - 1) / hopCalls);
    const int d0 = next - calls;   // 0..hopCalls-1

    const int target = juce::jlimit (0, hopCalls - 1, chooseTargetPhase (hopCalls));

    // Each extra silent call moves the heavy call one callback earlier.
    const auto live = liveCallbackIndex();
    const int shift = (int) (((live + (juce::uint64) d0) + (juce::uint64) hopCalls * 1000
                              - (juce::uint64) target) % (juce::uint64) hopCalls);
    processCalls (p, buf, shift, nullptr);

    return { hopCalls, target };
}

} // namespace stagerack
