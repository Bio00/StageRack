#pragma once

#include "../Engine/Session.h"
#include <set>

namespace stagerack
{

juce::Colour tempoColour (int index);

/** Computer-key taps: one tap per key press (the auto-repeat of a held key is ignored). */
class TapKeys
{
public:
    /** true = the key is a tap key (handled, also its repeats). */
    bool keyPressed (TempoEngine&, const juce::KeyPress&);
    void keyStateChanged();

private:
    std::set<int> held;
};

/** BPM readout with the beat LEDs. Drag up/down = change (Shift = fine), wheel = +-1 (Shift +-0.1),
    double click = type a value, click = onClick. */
class BpmDisplay final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    BpmDisplay (TempoEngine&, int index, bool compact);

    std::function<void()> onClick, onRightClick;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    TempoEngine& tempo;
    const int index;
    const bool compact;
    double dragStartBpm = 0.0;
    bool dragged = false;
    std::unique_ptr<juce::TextEditor> editor;
};

/** A TAP button that taps on mouse DOWN (not on release): the press is the beat. */
class TapButton final : public juce::TextButton
{
public:
    TapButton() : juce::TextButton ("TAP")  { setTriggeredOnMouseDown (true); }
};

/** One tempo in the top bar: a single TAP button in the tempo's colour that flashes on every beat,
    with its name and BPM. Press = tap (on the press), right click = menu (window, /2 x2, follow, remove). */
class TempoCell final : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    TempoCell (Session&, int index);
    std::function<void()> onOpenWindow;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void repaintDisplay()                          { repaint(); }
    const int index;

private:
    void showMenu();
    Session& session;
    bool pressed = false;
};

/** Master + the added tempos: tempo, follow / own, what a tap is worth, and the tap sources of each one
    (key, MIDI note/CC, MIDI clock, audio input as trigger or beat tracking). */
class TempoPanel final : public juce::Component,
                         private juce::Timer
{
public:
    explicit TempoPanel (Session&);
    ~TempoPanel() override;

    /** --render-racks: the "SEND" call-out of tempo i, on its own. */
    static std::unique_ptr<juce::Component> createSendPanel (Session&, int index);

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

    /** Refreshes every row from the engine (also called by tests). */
    void tick();

private:
    class Row;
    void timerCallback() override   { tick(); }
    void rebuildRows();
    juce::TextButton addButton { "+ ADD TEMPO" };
    juce::ToggleButton oscToggle;
    juce::TextEditor oscPort;
    juce::Label oscStatus;
    juce::uint32 lastOscCount = 0;
    int oscTicks = 0;
    void applyOsc();
    void updateOscStatus();
    void loadSettings();
    void saveSettings();

    Session& session;
    TapKeys keys;
    juce::OwnedArray<Row> rows;
    int keyLearn = -1;
    juce::uint32 lastChange = 0;

    juce::Label tapsLabel, resetLabel, roundLabel, rangeLabel, helpLabel;
    juce::ComboBox tapsBox, resetBox, roundBox, rangeBox;
    juce::ToggleButton alignToggle { "Beat on tap" };

    friend class Row;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TempoPanel)
};

class TempoWindow final : public juce::DocumentWindow
{
public:
    explicit TempoWindow (Session&);
    void closeButtonPressed() override { setVisible (false); }
};

} // namespace stagerack
