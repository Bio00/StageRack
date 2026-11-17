#include "Changelog.h"
#include "Updater.h"

namespace stagerack::update
{

static juce::String emDash()   { return juce::String::fromUTF8 ("\xe2\x80\x94"); }

juce::Array<ChangelogEntry> parseChangelog (const juce::String& markdown)
{
    juce::Array<ChangelogEntry> entries;
    juce::StringArray body;
    bool inEntry = false;

    const auto flush = [&]
    {
        if (inEntry)
            entries.getReference (entries.size() - 1).body = body.joinIntoString ("\n").trim();
        body.clear();
    };

    for (const auto& line : juce::StringArray::fromLines (markdown.replace ("\r", "")))
    {
        if (line.startsWith ("## "))
        {
            flush();
            const auto title = line.substring (3).trim();
            ChangelogEntry e;
            e.version = title.upToFirstOccurrenceOf (" ", false, false).trimCharactersAtStart ("v");
            // "— 2026-11-17" (em dash) or "- 2026-11-17"
            e.date = title.fromFirstOccurrenceOf (" ", false, false).trimCharactersAtStart (" -" + emDash()).trim();
            entries.add (e);
            inEntry = true;
        }
        else if (inEntry)
        {
            body.add (line);
        }
    }

    flush();
    return entries;
}

juce::String bundledChangelog()
{
    const auto file = juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                          .getChildFile ("Contents/Resources/CHANGELOG.md");
    return file.existsAsFile() ? file.loadFileAsString() : juce::String();
}

juce::String changesBetween (const juce::String& markdown, const juce::String& after, const juce::String& upTo)
{
    juce::String out;

    for (const auto& e : parseChangelog (markdown))
    {
        const bool wanted = after.isEmpty() ? compareVersions (e.version, upTo) == 0
                                            : compareVersions (e.version, after) > 0 && compareVersions (e.version, upTo) <= 0;
        if (wanted)
            out << "## " << e.version << (e.date.isNotEmpty() ? " " + emDash() + " " + e.date : juce::String())
                << "\n\n" << e.body << "\n\n";
    }

    return out.trim();
}

} // namespace stagerack::update
