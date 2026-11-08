#pragma once

#include <JuceHeader.h>

namespace stagerack
{

/** One meter a plugin publishes (gain reduction, input/output level...). */
struct PluginMeter
{
    juce::String name;
    int paramIndex = -1;            // index in getParameters() (VST3/CLAP/LV2)
    unsigned int auParamId = 0;     // AU: read straight from the Audio Unit (JUCE's cached value never updates for meters)
    bool isAU = false;
    float minValue = 0.0f, maxValue = 1.0f, restValue = 0.0f;
    juce::String unit;
    bool decibels = false, gainReduction = false;
};

/** Finds a plugin's meters. AU: the MeterReadOnly flag or read-only parameters, straight from the
    Audio Unit. VST3: the read-only flag (when the controller is reachable from the component).
    Otherwise, and for CLAP/LV2: non-automatable parameters with a meter-like name.
    GUI-state parameters some vendors also flag (markers, LED segments, "Dummy"...) are skipped. */
juce::Array<PluginMeter> findMeters (juce::AudioPluginInstance&);

/** Current value, 0..1 (message thread). */
float readMeter (juce::AudioPluginInstance&, const PluginMeter&);

/** Current value as text, e.g. "-3.2 dB". */
juce::String meterText (juce::AudioPluginInstance&, const PluginMeter&);

} // namespace stagerack
