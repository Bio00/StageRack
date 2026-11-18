#pragma once

#include <JuceHeader.h>

namespace stagerack::sandbox
{

/** True if the command line asks this process to be a sandbox child. */
bool isSandboxCommandLine (const juce::String& commandLine);

/** Child side: connects to the host and serves one plugin until the host goes away. Returns
    false if the command line was not a sandbox one (or the pipe failed). */
bool startSandboxChild (const juce::String& commandLine);

} // namespace stagerack::sandbox
