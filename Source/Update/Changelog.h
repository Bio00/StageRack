#pragma once

#include <JuceHeader.h>

namespace stagerack::update
{

/**
    The release notes: CHANGELOG.md (repository root, copied into Contents/Resources of the
    bundle). One section per version, "## <version> — <date>", newest first.
    scripts/release.sh publishes the same section as the GitHub release notes, so what the
    update dialog shows (from GitHub) and what the app shows after updating (from the bundle)
    are the same text.
*/
struct ChangelogEntry
{
    juce::String version, date, body;   // body = markdown under the "## " line
};

/** Sections of a changelog, in file order. Text before the first "## " is ignored. */
juce::Array<ChangelogEntry> parseChangelog (const juce::String& markdown);

/** CHANGELOG.md from the running bundle (empty if missing). */
juce::String bundledChangelog();

/** Markdown of the entries newer than `after` and not newer than `upTo` (newest first).
    `after` empty = only the `upTo` entry. */
juce::String changesBetween (const juce::String& markdown, const juce::String& after, const juce::String& upTo);

/** Settings key: the version whose notes the user has already been shown. */
inline constexpr const char* settingSeenVersion = "lastSeenVersion";

} // namespace stagerack::update
