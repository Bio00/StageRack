#pragma once

#include <JuceHeader.h>

namespace stagerack
{

/**
    Small pictures of plugin editors, for the slots' "mini view".

    Captured by asking the editor's native view hierarchy to draw itself into a bitmap
    (no screen-recording permission involved): when the plugin window is open, when it
    closes, and on request by briefly creating the editor in an invisible window.
    Editors that render with Metal/OpenGL may come out blank; looksBlank() detects that so
    the slot can show its fallback card instead.
*/
class PluginThumbnails
{
public:
    struct Entry
    {
        juce::Image image;
        int version = 0;
    };

    static PluginThumbnails& get();

    /** Latest picture of this plugin's editor (invalid if none yet). */
    Entry find (const juce::AudioPluginInstance*) const;

    /** Captures `editorComponent` (which must be on screen or in an invisible window). */
    void captureFrom (const juce::AudioPluginInstance*, juce::Component& editorComponent);

    /** Creates the editor invisibly, captures it after it had time to draw, then deletes it.
        Does nothing if the plugin has no editor or one is already active. */
    void requestOffscreen (juce::AudioPluginInstance&);

    /** Cancels a pending offscreen capture (call before opening the plugin's own window). */
    void cancelOffscreen (const juce::AudioPluginInstance*);

    /** Drops everything about a plugin that is about to be deleted. */
    void forget (const juce::AudioPluginInstance*);

    /** Drops everything (before the plugins are deleted, e.g. on quit or session load). */
    void reset();

    static bool looksBlank (const juce::Image&);

private:
    class OffscreenCapture;

    std::map<const juce::AudioPluginInstance*, Entry> entries;
    juce::OwnedArray<OffscreenCapture> pending;
    int nextVersion = 1;
};

} // namespace stagerack
