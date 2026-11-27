#pragma once

#include "../Engine/Session.h"
#include "TempoWindow.h"
#include "MidiTargets.h"
#include "../Engine/Features.h"
#include <array>

namespace stagerack
{

/** Top bar, in sections by function (left to right):
    session (logo, file, NEW/OPEN/SAVE) | scenes | live (MUTE ALL, automixer, GROUPS/TEMPO windows) |
    tap tempos (one cell per tempo + "+") |
    audio engine (DSP per core, device, PLUGINS/SETTINGS) | view (layout, theme).
    Every section has a minimum width: all of it stays visible down to the minimum window width. */
class TopBar final : public juce::Component,
                     public juce::SettableTooltipClient,
                     public MidiTargetProvider
{
public:
    void collectMidiTargets (std::vector<MidiTargetHit>&) override;
    std::function<void()> onMidiMap, onMidiWindow;
    explicit TopBar (Session&);

    /** Device problem to show instead of the device info ("Waiting for Dante..."), empty if none. */
    std::function<juce::String()> deviceStatus;
    std::function<void()> onNew, onOpen, onSave, onPlugins, onSettings, onToggleLayout, onAutomixView, onToggleTheme, onTempoView, onGroupsView;
    void setVerticalLayout (bool);

    void refresh();
    void updateMeters();

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

    static constexpr int preferredHeight = 76;

    //==============================================================================
    // Customisable (user request 2026-11-22): every block can be hidden and moved. The layout is an
    // app setting ("topBarLayout": keys in order, "-key" = hidden); blocks of a function switched off
    // in SETTINGS > Features are never shown.
    enum Item { itSession, itSnapshots, itMute, itAutomix, itGroups, itTempoWindow, itMidi, itTempos,
                itDsp, itDevice, itPluginsSettings, itView, itLayoutTheme, numItems };
    static juce::String itemKey (int);
    static juce::String itemName (int);
    static int itemFeature (int);                  // features::Feature, -1 = always available
    void setLayoutString (const juce::String&);
    juce::String getLayoutString() const;
    static juce::String defaultLayoutString();
    const std::vector<int>& getOrder() const noexcept   { return order; }
    bool isItemHidden (int i) const noexcept            { return hidden[(size_t) i]; }
    bool isItemShown (int i) const;                     // not hidden and its feature on
    void setItemHidden (int, bool);
    void moveItem (int item, int newIndex);
    void featuresChanged();                             // SETTINGS > Features changed: layout again
    void itemsChanged();                                // layout again + onLayoutChanged
    std::function<void (const juce::String&)> onLayoutChanged;   // save it
    std::function<void()> onDeviceClicked;                       // the audio device info: audio settings

    /** Edit mode: the blocks don't react, they are dragged to a new place (the Customize window is open). */
    void setCustomizing (bool);
    void showCustomizeWindow();
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void paintOverChildren (juce::Graphics&) override;

private:
    std::vector<int> order;
    std::array<bool, numItems> hidden {};
    std::array<juce::Rectangle<int>, numItems> itemArea;   // laid out blocks (edit mode, drag targets)
    bool customizing = false;
    int dragItem = -1, dropIndex = -1;
    int dropIndexAt (int x) const;
    std::vector<juce::Component*> componentsOf (int item);
    std::unique_ptr<juce::DocumentWindow> customizeWindow;
    void showContextMenu (juce::Point<int>);

    Session& session;

    juce::TextButton prevButton { juce::String::charToString (0x25C0) }, nextButton { juce::String::charToString (0x25B6) };
    juce::TextButton storeButton { "STORE" };
    juce::TextButton newButton { "NEW" }, openButton { "OPEN" }, saveButton { "SAVE" };
    juce::TextButton pluginsButton { "PLUGINS" }, settingsButton { "SETTINGS" }, layoutButton { "VERTICAL" }, themeButton { "LIGHT" };
    juce::TextButton automixButton { "AUTOMIX" }, automixViewButton { "AM METERS" };
    juce::TextButton masterMuteButton { "MUTE ALL" };
    juce::TextButton viewButton { "VIEW" };
    juce::TextButton groupsButton { "GROUPS" }, tempoButton { "TEMPO" }, addTempoButton { "+" };
    juce::TextButton midiMapButton { "MIDI MAP" }, midiButton { "MIDI" };
    juce::uint32 midiEventsSeen = 0, midiActivityAt = 0;
    juce::OwnedArray<TempoCell> tempoCells;
    void rebuildTempoCells();
    int blink = 0;

    juce::Rectangle<int> logoArea, sceneArea, cpuArea, deviceArea;
    juce::Array<int> separators;   // x of the lines between sections
    std::vector<float> threadLoads;
    float totalLoad = 0.0f;
    juce::String systemProblems;   // low power / heat / memory, refreshed once a second
    int healthTick = 29;
    juce::String shownCpuKey;   // what the DSP / dropout area shows: repainted only when it changes
    double secondsSinceLastDrop() const;
    int activeCores() const;   // 1 = single core (all racks on the audio thread), else cores in use
    juce::String dropoutTooltip() const;
};

} // namespace stagerack
