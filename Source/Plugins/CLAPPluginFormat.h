#pragma once

#include <JuceHeader.h>

namespace stagerack
{

/**
    CLAP hosting for JUCE (JUCE only hosts AU/VST2/VST3/LV2/LADSPA).

    Plugins are identified by bundle path (fileOrIdentifier) plus a hash of the CLAP
    plugin id (uniqueId), since one .clap bundle may contain several plugins.
*/
class CLAPPluginFormat final : public juce::AudioPluginFormat
{
public:
    CLAPPluginFormat() = default;

    static juce::String getFormatName()     { return "CLAP"; }
    juce::String getName() const override   { return getFormatName(); }

    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String& fileOrIdentifier) override;
    bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override;
    juce::String getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override;
    bool doesPluginStillExist (const juce::PluginDescription&) override;
    bool canScanForPlugins() const override             { return true; }
    bool isTrivialToScan() const override               { return false; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool recursive, bool allowAsync) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override;
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

private:
    void createPluginInstance (const juce::PluginDescription&, double initialSampleRate,
                               int initialBufferSize, PluginCreationCallback) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CLAPPluginFormat)
};

} // namespace stagerack
