#include "TempoWindow.h"
#include "Theme.h"

namespace stagerack
{

juce::Colour tempoColour (int index)
{
    switch (index)
    {
        case 1:  return theme::green;
        case 2:  return theme::amber;
        case 3:  return juce::Colour (0xfff26fd1);
        case 4:  return juce::Colour (0xffb98cff);
        default: return theme::accent;
    }
}

static juce::String bpmText (double bpm)
{
    const auto tenth = std::round (bpm * 10.0) / 10.0;
    return juce::String (bpm, std::abs (bpm - tenth) < 0.005 ? 1 : 2);
}

static juce::String keyName (int keyCode)
{
    if (keyCode == 0) return "none";
    if (keyCode == juce::KeyPress::spaceKey) return "Space";
    return juce::KeyPress (keyCode).getTextDescription().toUpperCase();
}

//==============================================================================
bool TapKeys::keyPressed (TempoEngine& tempo, const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;

    const auto code = key.getKeyCode();
    if (! tempo.isKeyUsed (code))
        return false;

    if (held.count (code) > 0)
        return true;   // auto-repeat of a held key: not a tap

    held.insert (code);
    tempo.handleKey (code, TempoEngine::nowSec());
    return true;
}

void TapKeys::keyStateChanged()
{
    for (auto it = held.begin(); it != held.end();)
        it = juce::KeyPress::isKeyCurrentlyDown (*it) ? std::next (it) : held.erase (it);
}

//==============================================================================
BpmDisplay::BpmDisplay (TempoEngine& t, int i, bool c) : tempo (t), index (i), compact (c)
{
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

void BpmDisplay::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    const auto colour = tempoColour (index);
    g.setColour (theme::display);
    g.fillRoundedRectangle (b, 5.0f);
    g.setColour (colour.withAlpha (0.55f));
    g.drawRoundedRectangle (b.reduced (0.5f), 5.0f, 1.0f);

    const auto now = TempoEngine::nowSec();
    const auto ppq = tempo.getBeatPosition (index, now);
    const auto beat = (int) std::fmod (std::floor (ppq), 4.0);
    const auto frac = ppq - std::floor (ppq);
    const int beatInBar = beat < 0 ? beat + 4 : beat;

    // beat LEDs: 4 dots, the current one lit for the first part of the beat (downbeat brighter)
    auto inner = b.reduced (compact ? 6.0f : 8.0f, 4.0f);
    auto leds = compact ? inner.removeFromLeft (10.0f) : inner.removeFromBottom (10.0f);
    const float d = compact ? 5.0f : 6.0f;

    for (int k = 0; k < 4; ++k)
    {
        const auto lit = k == beatInBar && frac < 0.22;
        juce::Rectangle<float> dot;

        if (compact) dot = { leds.getCentreX() - d * 0.5f, leds.getY() + (float) k * (leds.getHeight() - d) / 3.0f, d, d };
        else         dot = { leds.getX() + (float) k * (d + 5.0f), leds.getCentreY() - d * 0.5f, d, d };

        g.setColour (lit ? (k == 0 ? theme::red : colour) : theme::meterBg.brighter (0.25f));
        g.fillEllipse (dot);
    }

    if (! compact) inner.removeFromBottom (2.0f);

    const auto bpm = tempo.getBpm (index);

    // what drives it
    juce::String status;
    const auto src = tempo.getLastSource (index);
    const auto age = now - tempo.getLastEventTime (index);

    if (tempo.isClockRunning (index))               status = "CLOCK";
    else if (tempo.isTracking (index))              status = "AUDIO";
    else if (tempo.isFollower (index))              status = "M x" + juce::String (tempo.getConfig (index).ratio, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    else if (age < 2.5 && tempo.getTapCount (index) > 0) status = TempoEngine::sourceName (src) + " " + juce::String (tempo.getTapCount (index));

    if (compact && b.getWidth() < 70.0f)
        status.clear();   // no room: the digits come first

    g.setColour (theme::text);

    if (compact)
    {
        // narrow cell: whole BPM only; the font shrinks until the text fits
        const auto text = inner.getWidth() < 34.0f ? juce::String (juce::roundToInt (bpm)) : bpmText (bpm);
        auto area = status.isNotEmpty() ? inner.withTrimmedBottom (7.0f) : inner;
        float size = 17.0f;
        for (; size > 9.0f; size -= 0.5f)
            if (juce::GlyphArrangement::getStringWidth (theme::font (size, true), text) <= area.getWidth() - 1.0f)
                break;
        g.setFont (theme::font (size, true));
        g.drawText (text, area, juce::Justification::centred, false);
    }
    else
    {
        g.setFont (theme::font (22.0f, true));
        g.drawText (bpmText (bpm), inner, juce::Justification::centredLeft, false);
    }

    if (status.isNotEmpty())
    {
        g.setColour (colour);
        g.setFont (theme::font (compact ? 8.5f : 10.0f, true));
        if (compact) g.drawText (status, b.reduced (4.0f, 1.0f).removeFromBottom (9.0f), juce::Justification::centredRight, false);
        else         g.drawText (status, inner, juce::Justification::topRight, false);
    }
}

void BpmDisplay::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        dragged = true;   // no click, no drag
        if (onRightClick) onRightClick();
        return;
    }

    dragStartBpm = tempo.getBpm (index);
    dragged = false;
}

void BpmDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;

    if (std::abs (e.getDistanceFromDragStartY()) < 3 && ! dragged)
        return;

    dragged = true;
    const auto perPixel = e.mods.isShiftDown() ? 0.01 : 0.2;
    auto bpm = dragStartBpm - e.getDistanceFromDragStartY() * perPixel;
    bpm = std::round (bpm / (e.mods.isShiftDown() ? 0.01 : 0.1)) * (e.mods.isShiftDown() ? 0.01 : 0.1);
    tempo.setBpm (index, juce::jlimit (20.0, 400.0, bpm));
    repaint();
}

void BpmDisplay::mouseUp (const juce::MouseEvent& e)
{
    if (! dragged && ! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && onClick)
        onClick();
}

void BpmDisplay::mouseDoubleClick (const juce::MouseEvent&)
{
    editor = std::make_unique<juce::TextEditor>();
    editor->setJustification (juce::Justification::centred);
    editor->setFont (theme::font (16.0f, true));
    editor->setInputRestrictions (6, "0123456789.,");
    editor->setText (juce::String (tempo.getBpm (index), 2), false);
    editor->selectAll();
    addAndMakeVisible (*editor);
    editor->setBounds (getLocalBounds().reduced (2));

    juce::Component::SafePointer<BpmDisplay> safe (this);
    auto finish = [safe] (bool apply)
    {
        if (safe == nullptr || safe->editor == nullptr) return;
        const auto v = safe->editor->getText().replaceCharacter (',', '.').getDoubleValue();
        if (apply && v >= 20.0 && v <= 400.0)
            safe->tempo.setBpm (safe->index, v);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->editor.reset(); safe->repaint(); } });
    };
    editor->onReturnKey = [finish] { finish (true); };
    editor->onFocusLost = [finish] { finish (true); };
    editor->onEscapeKey = [finish] { finish (false); };
    editor->grabKeyboardFocus();
}

void BpmDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto dir = (w.deltaY != 0.0f ? w.deltaY : w.deltaX) > 0.0f ? 1.0 : -1.0;
    tempo.setBpm (index, juce::jlimit (20.0, 400.0, tempo.getBpm (index) + dir * (e.mods.isShiftDown() ? 0.1 : 1.0)));
    repaint();
}

//==============================================================================
TempoCell::TempoCell (Session& s, int i)
    : index (i), session (s)
{
    setRepaintsOnMouseActivity (true);
}

void TempoCell::resized()
{
    const auto name = session.getEngine().tempo.getConfig (index).name;
    const juce::String how = index == 0 ? "key T, " : "";
    setTooltip ("Tap " + name + " (on the press)" + (index > 0 ? ". If it follows the master, tapping makes it independent." : "")
                + "\nOther tap sources (" + how + "MIDI, clock, audio) in the Tempo window. Right click = more");
}

void TempoCell::paint (juce::Graphics& g)
{
    auto& tempo = session.getEngine().tempo;
    const auto colour = tempoColour (index);
    auto b = getLocalBounds().toFloat().reduced (0.5f);

    // beat: LED + fill that fade out over the first quarter of the beat; how much = SETTINGS > TAP beat flash
    const auto ppq = tempo.getBeatPosition (index, TempoEngine::nowSec());
    const auto frac = (float) (ppq - std::floor (ppq));
    const auto pulse = juce::jmax (0.0f, 1.0f - frac / 0.25f);
    static const float fillAmount[] = { 0.0f, 0.10f, 0.30f, 0.60f, 1.0f };
    const int level = juce::jlimit (0, 4, theme::tapFlash);
    const auto glow = level > 0 ? pulse * pulse : 0.0f;
    const auto fill = pressed ? juce::jmax (0.4f, fillAmount[level] * glow) : fillAmount[level] * glow;

    g.setColour (theme::display);
    g.fillRoundedRectangle (b, 4.0f);
    g.setColour (colour.withAlpha (juce::jmax (0.06f + (isMouseOver() ? 0.06f : 0.0f), fill)));
    g.fillRoundedRectangle (b, 4.0f);
    g.setColour (colour.withAlpha (0.45f + 0.55f * glow));
    g.drawRoundedRectangle (b, 4.0f, level >= 3 ? 1.0f + glow : 1.0f);

    // on a strong fill the text needs the contrasting colour
    const bool onColour = fill > 0.55f;
    const auto contrast = colour.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white;

    auto inner = b.reduced (4.0f, 3.0f);
    auto top = inner.removeFromTop (11.0f);

    const float d = 5.0f;
    const juce::Rectangle<float> led (top.getRight() - d, top.getCentreY() - d * 0.5f, d, d);
    g.setColour (onColour ? contrast.withAlpha (0.8f) : theme::meterBg.brighter (0.2f).interpolatedWith (colour, glow));
    g.fillEllipse (led);

    const auto label = index == 0 ? juce::String ("TAP") : TempoEngine::shortName (index);
    g.setColour (onColour ? contrast : theme::light ? colour.darker (0.7f) : colour.brighter (0.2f));
    g.setFont (theme::font (9.0f, true));
    g.drawText (label, top.withTrimmedRight (d + 2.0f), juce::Justification::centredLeft, false);

    // BPM: one decimal if it fits, whole otherwise
    const auto bpm = tempo.getBpm (index);
    auto text = bpmText (bpm);
    float size = 14.0f;
    auto fits = [&] { return juce::GlyphArrangement::getStringWidth (theme::font (size, true), text) <= inner.getWidth(); };
    while (! fits() && size > 11.0f) size -= 0.5f;
    if (! fits()) { text = juce::String (juce::roundToInt (bpm)); size = 14.0f; while (! fits() && size > 9.0f) size -= 0.5f; }
    g.setColour (onColour ? contrast : theme::text);
    g.setFont (theme::font (size, true));
    g.drawText (text, inner, juce::Justification::centred, false);
}

void TempoCell::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { showMenu(); return; }
    session.getEngine().tempo.tapNow (index, TempoEngine::srcButton);
    pressed = true;
    repaint();
}

void TempoCell::mouseUp (const juce::MouseEvent&)
{
    pressed = false;
    repaint();
}

void TempoCell::showMenu()
{
    juce::Component::SafePointer<TempoCell> safe (this);
    auto& tempo = session.getEngine().tempo;
    juce::PopupMenu m;
    m.addSectionHeader (tempo.getConfig (index).name + "  (" + TempoEngine::shortName (index) + ")");
    m.addItem ("Tempo window...", [safe] { if (safe != nullptr && safe->onOpenWindow) safe->onOpenWindow(); });
    m.addItem (juce::String::fromUTF8 ("Half tempo  (\xc3\xb7" "2)"), [safe] { if (safe != nullptr) safe->session.getEngine().tempo.multiplyBpm (safe->index, 0.5); });
    m.addItem ("Double tempo  (x2)", [safe] { if (safe != nullptr) safe->session.getEngine().tempo.multiplyBpm (safe->index, 2.0); });

    if (index > 0)
    {
        m.addItem (juce::PopupMenu::Item ("Follow the master").setTicked (tempo.isFollower (index))
                       .setAction ([safe] { if (safe != nullptr) { safe->session.getEngine().tempo.link (safe->index, 1.0); safe->session.markDirty(); } }));
        m.addSeparator();
        auto& session = this->session;
        const int i = index;
        m.addItem ("Remove " + TempoEngine::shortName (index) + "  (its racks go to the master)",
                   [&session, i] { session.removeTempo (i); });
    }

    m.addSeparator();
    m.addItem ("Add a tempo", tempo.getNumTempos() < TempoEngine::maxTempos, false, [&s = session] { s.addTempo(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}

//==============================================================================
/** Click = learn, right click = clear. */
struct LearnButton final : juce::TextButton
{
    std::function<void()> onClear;
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onClear) onClear(); return; }
        juce::TextButton::mouseDown (e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu()) juce::TextButton::mouseUp (e);
    }
};

//==============================================================================
/** Where tempo `index` is sent: every beat or your taps, MIDI (note / CC, clock), OSC. Opens in a call-out from SEND. */
class SendPanel final : public juce::Component,
                        private juce::Timer
{
public:
    SendPanel (Session& s, int i) : session (s), tempo (s.getEngine().tempo), index (i)
    {
        auto label = [this] (juce::Label& l, const juce::String& t)
        {
            l.setText (t, juce::dontSendNotification);
            l.setFont (theme::font (11.0f, true));
            l.setColour (juce::Label::textColourId, theme::textDim);
            addAndMakeVisible (l);
        };

        label (title, "SEND " + tempo.getConfig (index).name.toUpperCase() + " OUT");
        title.setColour (juce::Label::textColourId, tempoColour (index));
        label (whatLabel, "Send");
        label (midiLabel, "MIDI out");
        label (msgLabel, "Message");
        label (oscLabel, "OSC out");
        label (addrLabel, "Addresses");

        what.addItem ("Every beat (continuous)", 1);
        what.addItem ("My taps only, as I give them", 2);
        what.setTooltip ("Every beat: the other side follows the tempo (lights on the beat, a desk's BPM).\n"
                         "My taps: each of your taps (button, key, MIDI, OSC) is passed on, e.g. to a lighting desk's own tap key.");
        what.onChange = [this] { apply ([this] (auto& c) { c.outWhat = what.getSelectedId() - 1; }); };
        addAndMakeVisible (what);

        midiDevice.setTooltip ("MIDI output (interface, IAC bus, network session...)");
        midiDevice.onChange = [this]
        {
            const auto k = midiDevice.getSelectedId() - 2;
            apply ([this, k] (auto& c)
            {
                if (juce::isPositiveAndBelow (k, devices.size())) { c.midiOutId = devices[k].identifier; c.midiOutName = devices[k].name; }
                else if (k < 0)                                      { c.midiOutId = {}; c.midiOutName = {}; }
            });
        };
        addAndMakeVisible (midiDevice);

        msgKind.addItem ("No note / CC", 1);
        msgKind.addItem ("Note", 2);
        msgKind.addItem ("CC (127, then 0)", 3);
        msgKind.onChange = [this] { apply ([this] (auto& c) { c.midiOutKind = msgKind.getSelectedId() - 1; }); number.updateText(); };
        addAndMakeVisible (msgKind);

        number.setRange (0, 127, 1);
        number.setSliderStyle (juce::Slider::IncDecButtons);
        number.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 72, 22);
        number.textFromValueFunction = [this] (double v)
        {
            const int n = (int) v;
            return msgKind.getSelectedId() == 2 ? juce::MidiMessage::getMidiNoteName (n, true, true, 3) + " (" + juce::String (n) + ")"
                                                : "CC " + juce::String (n);
        };
        number.valueFromTextFunction = [] (const juce::String& t)
        {
            const auto inBrackets = t.fromFirstOccurrenceOf ("(", false, false).upToFirstOccurrenceOf (")", false, false);
            return (double) (inBrackets.isNotEmpty() ? inBrackets : t).retainCharacters ("0123456789").getIntValue();
        };
        number.setTooltip ("Note or CC number");
        number.onValueChange = [this] { apply ([this] (auto& c) { c.midiOutNumber = (int) number.getValue(); }); };
        addAndMakeVisible (number);

        for (int ch = 1; ch <= 16; ++ch) channel.addItem ("ch " + juce::String (ch), ch);
        channel.onChange = [this] { apply ([this] (auto& c) { c.midiOutChannel = juce::jmax (1, channel.getSelectedId()); }); };
        addAndMakeVisible (channel);

        clock.setButtonText ("MIDI clock out (24 per beat, Start on a downbeat)");
        clock.onClick = [this] { apply ([this] (auto& c) { c.midiOutClock = clock.getToggleState(); }); };
        addAndMakeVisible (clock);

        oscOn.setButtonText ("Send OSC to");
        oscOn.onClick = [this] { apply ([this] (auto& c) { c.oscOut = oscOn.getToggleState(); }); };
        addAndMakeVisible (oscOn);

        auto editor = [this] (juce::TextEditor& e, const juce::String& tip, std::function<void (TempoEngine::Config&, const juce::String&)> set)
        {
            e.setTooltip (tip);
            e.onReturnKey = e.onFocusLost = [this, &e, set] { const auto text = e.getText().trim(); apply ([&] (auto& c) { set (c, text); }); };
            addAndMakeVisible (e);
        };

        editor (host, "IP address of the receiver (lighting desk, video computer...). 127.0.0.1 = this Mac",
                [] (auto& c, const juce::String& t) { c.oscOutHost = t.isNotEmpty() ? t : juce::String ("127.0.0.1"); });
        port.setInputRestrictions (5, "0123456789");
        editor (port, "UDP port the receiver listens on",
                [] (auto& c, const juce::String& t) { c.oscOutPort = juce::jlimit (1, 65535, t.getIntValue() > 0 ? t.getIntValue() : 9000); });
        editor (beatAddr, "Sent on every beat (or tap) with the beat in the bar 1-4. Empty = " + TempoOutput::defaultBeatAddress (index),
                [] (auto& c, const juce::String& t) { c.oscOutBeat = t; });
        editor (bpmAddr, "Sent with the tempo in BPM (float) when it changes, and every 2 s. Empty = " + TempoOutput::defaultBpmAddress (index),
                [] (auto& c, const juce::String& t) { c.oscOutBpm = t; });
        beatAddr.setTextToShowWhenEmpty (TempoOutput::defaultBeatAddress (index) + "  <1-4>", theme::textDim);
        bpmAddr.setTextToShowWhenEmpty (TempoOutput::defaultBpmAddress (index) + "  <bpm>", theme::textDim);

        status.setFont (theme::font (11.0f));
        addAndMakeVisible (status);

        refresh();
        setSize (520, 300);
        startTimerHz (5);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        title.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        auto row = [&r] { auto x = r.removeFromTop (26); r.removeFromTop (6); return x; };

        auto a = row(); whatLabel.setBounds (a.removeFromLeft (76)); what.setBounds (a.removeFromLeft (260));
        auto b = row(); midiLabel.setBounds (b.removeFromLeft (76)); midiDevice.setBounds (b);
        auto m = row(); msgLabel.setBounds (m.removeFromLeft (76)); msgKind.setBounds (m.removeFromLeft (150)); m.removeFromLeft (6);
        number.setBounds (m.removeFromLeft (140)); m.removeFromLeft (6); channel.setBounds (m.removeFromLeft (80));
        auto k = row(); k.removeFromLeft (76); clock.setBounds (k);
        auto o = row(); oscLabel.setBounds (o.removeFromLeft (76)); oscOn.setBounds (o.removeFromLeft (110)); host.setBounds (o.removeFromLeft (170)); o.removeFromLeft (6); port.setBounds (o.removeFromLeft (70));
        auto d = row(); addrLabel.setBounds (d.removeFromLeft (76)); beatAddr.setBounds (d.removeFromLeft (205)); d.removeFromLeft (6); bpmAddr.setBounds (d);
        status.setBounds (r);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::bg); }

private:
    void apply (std::function<void (TempoEngine::Config&)> fn)
    {
        auto c = tempo.getConfig (index);
        fn (c);
        tempo.setConfig (index, c);
        session.markDirty();
        session.getEngine().tempoOut.update();
        refresh();
    }

    void refresh()
    {
        const auto c = tempo.getConfig (index);
        what.setSelectedId (c.outWhat + 1, juce::dontSendNotification);

        devices = juce::MidiOutput::getAvailableDevices();
        midiDevice.clear (juce::dontSendNotification);
        midiDevice.addItem ("None", 1);
        int sel = 1;
        for (int k = 0; k < devices.size(); ++k)
        {
            midiDevice.addItem (devices[k].name, k + 2);
            if (devices[k].identifier == c.midiOutId || (sel == 1 && devices[k].name == c.midiOutName && c.midiOutName.isNotEmpty())) sel = k + 2;
        }
        if (sel == 1 && c.midiOutId.isNotEmpty())
        {
            midiDevice.addItem (c.midiOutName + " (not connected)", 1000);
            sel = 1000;
        }
        midiDevice.setSelectedId (sel, juce::dontSendNotification);

        const bool midi = c.midiOutId.isNotEmpty();
        msgKind.setSelectedId (c.midiOutKind + 1, juce::dontSendNotification);
        number.setValue (c.midiOutNumber, juce::dontSendNotification);
        number.updateText();
        channel.setSelectedId (c.midiOutChannel, juce::dontSendNotification);
        clock.setToggleState (c.midiOutClock, juce::dontSendNotification);
        for (auto* comp : std::initializer_list<juce::Component*> { &msgKind, &number, &channel, &clock })
            comp->setEnabled (midi);
        number.setEnabled (midi && c.midiOutKind != TempoEngine::midiNone);
        channel.setEnabled (number.isEnabled());

        oscOn.setToggleState (c.oscOut, juce::dontSendNotification);
        if (! host.hasKeyboardFocus (true))     host.setText (c.oscOutHost, false);
        if (! port.hasKeyboardFocus (true))     port.setText (juce::String (c.oscOutPort), false);
        if (! beatAddr.hasKeyboardFocus (true)) beatAddr.setText (c.oscOutBeat, false);
        if (! bpmAddr.hasKeyboardFocus (true))  bpmAddr.setText (c.oscOutBpm, false);
        timerCallback();
    }

    void timerCallback() override
    {
        const auto problem = session.getEngine().tempoOut.getProblem (index);
        const auto c = tempo.getConfig (index);
        juce::String s;
        juce::Colour col = theme::textDim;

        if (problem.isNotEmpty())   { s = problem; col = theme::red; }
        else if (! c.sendsOut())    { s = "Not sent. Pick a MIDI output with a note / CC or clock, or turn OSC on."; }
        else                        { s = "Sending: " + juce::String ((int) session.getEngine().tempoOut.getSentCount (index)) + (c.outWhat == TempoEngine::outTaps ? " taps" : " beats") + " so far"; col = theme::green; }

        status.setText (s, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, col);
    }

    Session& session;
    TempoEngine& tempo;
    const int index;
    juce::Array<juce::MidiDeviceInfo> devices;
    juce::Label title, whatLabel, midiLabel, msgLabel, oscLabel, addrLabel, status;
    juce::ComboBox what, midiDevice, msgKind, channel;
    juce::Slider number;
    juce::ToggleButton clock, oscOn;
    juce::TextEditor host, port, beatAddr, bpmAddr;
};

static const double followRatios[] = { 0.0, 1.0, 2.0, 0.5, 1.5, 2.0 / 3.0, 4.0 / 3.0, 0.75 };
static const char* followNames[] = { "Own tempo", "= Master", "Master x2", "Master x1/2", "Master x3/2", "Master x2/3", "Master x4/3", "Master x3/4" };

class TempoPanel::Row final : public juce::Component
{
public:
    Row (TempoPanel& p, int i)
        : panel (p), tempo (p.session.getEngine().tempo), index (i), bpm (tempo, i, false)
    {
        nameLabel.setEditable (false, true, false);
        nameLabel.setFont (theme::font (13.0f, true));
        nameLabel.setTooltip ("Double click to rename");
        nameLabel.onTextChange = [this] { apply ([this] (auto& c) { c.name = nameLabel.getText().trim(); }); };
        addAndMakeVisible (nameLabel);

        bpm.setTooltip ("Drag up/down to change (Shift = fine), wheel = +-1 BPM, double click = type the tempo");
        addAndMakeVisible (bpm);

        tap.setTooltip ("Tap the beat (works on the press). Tapping a tempo that follows the master makes it independent.");
        tap.onClick = [this] { tempo.tapNow (index, TempoEngine::srcButton); };
        addAndMakeVisible (tap);

        halfButton.setButtonText (juce::String::fromUTF8 ("\xc3\xb7" "2"));
        halfButton.setTooltip ("Half tempo (a follower: half the ratio)");
        halfButton.onClick = [this] { tempo.multiplyBpm (index, 0.5); panel.session.markDirty(); };
        doubleButton.setTooltip ("Double tempo (a follower: double the ratio)");
        doubleButton.onClick = [this] { tempo.multiplyBpm (index, 2.0); panel.session.markDirty(); };
        addAndMakeVisible (halfButton);
        addAndMakeVisible (doubleButton);

        for (int k = 0; k < (int) std::size (followRatios); ++k)
            followBox.addItem (followNames[k], k + 1);
        followBox.setTooltip ("Own tempo, or follow the master tempo times a ratio (tap the master: this one moves too)");
        followBox.onChange = [this]
        {
            const auto id = followBox.getSelectedId();
            if (id <= 0) return;
            const auto r = followRatios[id - 1];
            if (r > 0.0) tempo.link (index, r);
            else         apply ([] (auto& c) { c.ratio = 0.0; });
            panel.session.markDirty();
        };
        if (index > 0) addAndMakeVisible (followBox);

        for (int v = 0; v <= TempoEngine::tapBar; ++v)
            tapValueBox.addItem ("Tap = " + TempoEngine::tapValueName (v), v + 1);
        tapValueBox.setTooltip ("What one tap is worth: tap eighths, quarters, half notes or only the downbeats (Bar: also puts the bar on the tap)");
        tapValueBox.onChange = [this] { apply ([this] (auto& c) { c.tapValue = tapValueBox.getSelectedId() - 1; }); };
        addAndMakeVisible (tapValueBox);

        sceneToggle.setButtonText ("Scenes");
        sceneToggle.setTooltip ("Scenes store this tempo and recall it (song tempo per scene). Not while MIDI clock or the drums drive it.");
        sceneToggle.onClick = [this] { apply ([this] (auto& c) { c.sceneRecall = sceneToggle.getToggleState(); }); };
        addAndMakeVisible (sceneToggle);

        // --- sources
        keyButton.setTooltip ("Computer key that taps this tempo (StageRack window in front). Click, then press the key; right click = none.");
        keyButton.onClick = [this]
        {
            panel.keyLearn = panel.keyLearn == index ? -1 : index;
            panel.grabKeyboardFocus();
            panel.tick();
        };
        keyButton.onClear = [this] { apply ([] (auto& c) { c.keyCode = 0; }); };
        addAndMakeVisible (keyButton);

        midiButton.setTooltip ("MIDI note or CC that taps (footswitch, pad, controller button). Click, then play it; right click = none. "
                               "MIDI inputs are enabled in SETTINGS.");
        midiButton.onClick = [this]
        {
            tempo.learnMidi (tempo.getLearningMidi() == index ? -1 : index);
            panel.tick();
        };
        midiButton.onClear = [this] { apply ([] (auto& c) { c.midiKind = TempoEngine::midiNone; }); };
        addAndMakeVisible (midiButton);

        oscButton.setTooltip ("OSC address that taps this tempo or sets its BPM (TouchOSC, Stream Deck, QLab, Ableton, a lighting desk). "
                              "Click = learn (then send it), right click = none. OSC in is turned on below. "
                              "Fixed addresses work without learning: /stagerack/tap, /stagerack/bpm <n> ... (see the help).");
        oscButton.onClick = [this]
        {
            juce::Component::SafePointer<Row> safe (this);
            juce::PopupMenu m;
            const auto c = tempo.getConfig (index);
            m.addItem ("Learn tap (press the OSC button)", [safe] { if (safe) { safe->tempo.learnOsc (safe->index, TempoEngine::oscLearnTap); safe->panel.tick(); } });
            m.addItem ("Learn BPM (send the tempo number)", [safe] { if (safe) { safe->tempo.learnOsc (safe->index, TempoEngine::oscLearnBpm); safe->panel.tick(); } });
            if (tempo.getLearningOsc() == index)
                m.addItem ("Cancel learn", [safe] { if (safe) { safe->tempo.learnOsc (-1, 0); safe->panel.tick(); } });
            m.addSeparator();
            m.addItem ("Clear tap" + (c.oscTap.isNotEmpty() ? "  (" + c.oscTap + ")" : juce::String()), c.oscTap.isNotEmpty(), false,
                       [safe] { if (safe) safe->apply ([] (auto& cfg) { cfg.oscTap = {}; }); });
            m.addItem ("Clear BPM" + (c.oscBpm.isNotEmpty() ? "  (" + c.oscBpm + ")" : juce::String()), c.oscBpm.isNotEmpty(), false,
                       [safe] { if (safe) safe->apply ([] (auto& cfg) { cfg.oscBpm = {}; }); });
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&oscButton));
        };
        oscButton.onClear = [this] { apply ([] (auto& c) { c.oscTap = {}; c.oscBpm = {}; }); };
        addAndMakeVisible (oscButton);

        clockButton.setClickingTogglesState (true);
        clockButton.setTooltip ("Follow MIDI clock in (24 per beat). Start / Song Position put the beat in phase with the sender.");
        clockButton.onClick = [this] { apply ([this] (auto& c) { c.midiClock = clockButton.getToggleState(); }); };
        addAndMakeVisible (clockButton);

        audioBox.setTooltip ("Audio that taps: a device input (click track, pad, kick mic...) or a rack: before its plugins, "
                             "after its plugins (pre fader) or after its fader");
        audioBox.onChange = [this]
        {
            const auto id = audioBox.getSelectedId();
            apply ([id] (auto& c)
            {
                if (id >= 1000) { c.audioPoint = (id - 1000) % 4; c.audioRackId = (id - 1000) / 4; c.audioChannel = -1; }
                else            { c.audioPoint = TempoEngine::pointDeviceInput; c.audioChannel = id - 2; }
            });
        };

        for (auto [sec, text] : { std::pair<int, const char*> { 0, "After a tap: keep my tap" }, { 4, "After a tap: back to the audio after 4 s" },
                                  { 8, "After a tap: back to the audio after 8 s" }, { 16, "After a tap: back to the audio after 16 s" },
                                  { 30, "After a tap: back to the audio after 30 s" } })
            afterTapBox.addItem (text, sec + 1);
        afterTapBox.setTooltip ("Your tap (or a tempo you set, a scene, OSC) always wins over the audio: the audio may then only fine-tune it "
                                "(within 4%: tempo decimals, beat on the drums).\n"
                                "Keep my tap: it stays until you tap again, even if the audio says otherwise.\n"
                                "Back to the audio: if the audio keeps saying another tempo for that long, it takes over (your tap was a mistake).");
        afterTapBox.onChange = [this] { apply ([this] (auto& c) { c.returnToAudioSec = afterTapBox.getSelectedId() - 1; }); };
        addAndMakeVisible (afterTapBox);

        for (auto pct : { 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25 })
            windowBox.addItem (juce::String::fromUTF8 ("\xc2\xb1") + juce::String (pct) + "%", pct);
        windowBox.setTooltip ("Tolerance: after your tap the audio may fine-tune the tempo only within this much of it "
                              "(tempo decimals, beat on the drums). Further away it is ignored (or takes over after the time chosen beside).");
        windowBox.onChange = [this] { apply ([this] (auto& c) { c.audioWindowPct = (float) windowBox.getSelectedId(); }); };
        addAndMakeVisible (windowBox);

        sendButton.setTooltip ("Send this tempo out: every beat or your taps, as a MIDI note / CC, MIDI clock and / or OSC "
                               "(lighting desk, video, another computer...)");
        sendButton.onClick = [this]
        {
            auto* top = panel.getTopLevelComponent();
            juce::CallOutBox::launchAsynchronously (std::make_unique<SendPanel> (panel.session, index),
                                                    top->getLocalArea (this, sendButton.getBounds()), top);
        };
        addAndMakeVisible (sendButton);
        addAndMakeVisible (audioBox);

        modeBox.addItem ("Every hit = tap", 1);
        modeBox.addItem ("Follow the beat", 2);
        modeBox.setTooltip ("Every hit = tap: each transient is a tap (click track, pad).\n"
                            "Follow the beat: tracks the tempo of a drum (kick), staying within half an octave of the current tempo "
                            "(tap once to give the octave).");
        modeBox.onChange = [this] { apply ([this] (auto& c) { c.audioMode = modeBox.getSelectedId() - 1; }); };
        addAndMakeVisible (modeBox);

        threshold.setSliderStyle (juce::Slider::LinearBar);
        threshold.setRange (-60.0, 0.0, 1.0);
        threshold.setTextValueSuffix (" dB");
        threshold.setDoubleClickReturnValue (true, -30.0);
        threshold.setTooltip ("Trigger threshold of the audio input (the bar beside shows its level, it flashes on every hit)");
        threshold.onValueChange = [this] { apply ([this] (auto& c) { c.audioThresholdDb = (float) threshold.getValue(); }, false); };
        threshold.onDragEnd = [this] { panel.session.markDirty(); };
        addAndMakeVisible (threshold);

        removeButton.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x95"));
        removeButton.setTooltip ("Remove this tempo (its racks go back to the master)");
        removeButton.onClick = [this] { panel.session.removeTempo (index); };   // rows are rebuilt on the next tick
        if (index > 0) addAndMakeVisible (removeButton);

        lookAndFeelChanged();
        refresh();
    }

    void apply (std::function<void (TempoEngine::Config&)> fn, bool dirty = true)
    {
        auto c = tempo.getConfig (index);
        fn (c);
        tempo.setConfig (index, c);
        if (dirty) panel.session.markDirty();
        refresh();
    }

    void lookAndFeelChanged() override
    {
        const auto c = tempoColour (index);
        tap.setColour (juce::TextButton::buttonColourId, c.withAlpha (0.35f));
        tap.setColour (juce::TextButton::buttonOnColourId, c);
        clockButton.setColour (juce::TextButton::buttonOnColourId, c.withAlpha (0.8f));
        nameLabel.setColour (juce::Label::textColourId, theme::text);
        threshold.setColour (juce::Slider::trackColourId, c.withAlpha (0.35f));
    }

    static int rackItemId (int rackId, int point)  { return 1000 + rackId * 4 + point; }

    void fillAudioInputs()
    {
        juce::StringArray names;
        auto& engine = panel.session.getEngine();
        if (auto* dev = engine.getDeviceManager().getCurrentAudioDevice())
            names = dev->getInputChannelNames();

        const auto c = tempo.getConfig (index);
        juce::StringArray racks;   // "id|name"
        for (int r = 0; r < engine.getNumRacks(); ++r)
            racks.add (juce::String (engine.getRack (r).getId()) + "|" + engine.getRack (r).name);

        const auto signature = names.joinIntoString ("\n") + "#" + racks.joinIntoString ("\n") + "#" + juce::String (c.audioChannel) + "/" + juce::String (c.audioRackId);
        if (signature == audioSignature)
            return;

        audioSignature = signature;
        audioBox.clear (juce::dontSendNotification);
        auto* root = audioBox.getRootMenu();
        root->addItem (1, "Audio: off");

        // device inputs (no device yet: still offer channels, and keep the saved one)
        juce::PopupMenu inputs;
        const int n = juce::jmax (names.isEmpty() ? 32 : names.size(), c.audioChannel + 1);
        for (int ch = 0; ch < n; ++ch)
            inputs.addItem (ch + 2, "IN " + juce::String (ch + 1) + (ch < names.size() && names[ch].isNotEmpty() && names[ch] != juce::String (ch + 1) ? "  " + names[ch] : juce::String()));
        root->addSubMenu ("Device input", inputs);

        // racks: before the plugins, after the plugins, after the fader
        juce::PopupMenu rackMenu;
        bool savedFound = c.audioRackId < 0;
        for (const auto& item : racks)
        {
            const int id = item.upToFirstOccurrenceOf ("|", false, false).getIntValue();
            const auto name = item.fromFirstOccurrenceOf ("|", false, false);
            juce::PopupMenu points;
            points.addItem (rackItemId (id, TempoEngine::pointPrePlugins), name + ": before plugins");
            points.addItem (rackItemId (id, TempoEngine::pointPostPlugins), name + ": after plugins (pre fader)");
            points.addItem (rackItemId (id, TempoEngine::pointPostFader), name + ": post fader");
            rackMenu.addSubMenu (name, points);
            savedFound = savedFound || id == c.audioRackId;
        }

        if (! savedFound)
            for (int p = 1; p <= 3; ++p)
                rackMenu.addItem (rackItemId (c.audioRackId, p), "(rack removed)" + juce::String (p == c.audioPoint ? "" : " " + juce::String (p)));

        root->addSubMenu ("Rack", rackMenu, ! racks.isEmpty() || ! savedFound);
    }

    void refresh()
    {
        const auto c = tempo.getConfig (index);
        fillAudioInputs();

        if (! nameLabel.isBeingEdited())
            nameLabel.setText (c.name, juce::dontSendNotification);

        if (index > 0)
        {
            int id = 0;
            for (int k = 0; k < (int) std::size (followRatios); ++k)
                if (std::abs (followRatios[k] - c.ratio) < 1e-6)
                    id = k + 1;

            if (id > 0) followBox.setSelectedId (id, juce::dontSendNotification);
            else        followBox.setText ("Master x" + juce::String (c.ratio, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."), juce::dontSendNotification);
        }

        tapValueBox.setSelectedId (c.tapValue + 1, juce::dontSendNotification);
        sceneToggle.setToggleState (c.sceneRecall, juce::dontSendNotification);

        keyButton.setButtonText (panel.keyLearn == index ? juce::String ("Press a key...") : "Key: " + keyName (c.keyCode));
        keyButton.setColour (juce::TextButton::buttonColourId, panel.keyLearn == index ? theme::amber.withAlpha (0.6f) : theme::slot);

        const bool learning = tempo.getLearningMidi() == index;
        juce::String midi = "MIDI: none";
        if (c.midiKind == TempoEngine::midiNote)
            midi = "Note " + juce::MidiMessage::getMidiNoteName (c.midiNumber, true, true, 3) + " (" + juce::String (c.midiNumber) + ")";
        else if (c.midiKind == TempoEngine::midiCC)
            midi = "CC " + juce::String (c.midiNumber);
        if (c.midiKind != TempoEngine::midiNone)
            midi << (c.midiChannel == 0 ? juce::String() : " ch" + juce::String (c.midiChannel));
        midiButton.setButtonText (learning ? juce::String ("Play note / CC...") : midi);
        midiButton.setColour (juce::TextButton::buttonColourId, learning ? theme::amber.withAlpha (0.6f) : theme::slot);

        const bool oscLearn = tempo.getLearningOsc() == index;
        juce::String osc = "OSC: none";
        if (c.oscTap.isNotEmpty() || c.oscBpm.isNotEmpty())
            osc = "OSC " + (c.oscTap.isNotEmpty() ? c.oscTap : juce::String()) + (c.oscBpm.isNotEmpty() ? (c.oscTap.isNotEmpty() ? " + BPM" : "BPM " + c.oscBpm) : juce::String());
        oscButton.setButtonText (oscLearn ? (tempo.getLearningOscKind() == TempoEngine::oscLearnBpm ? juce::String ("Send the BPM...") : juce::String ("Send OSC...")) : osc);
        oscButton.setColour (juce::TextButton::buttonColourId, oscLearn ? theme::amber.withAlpha (0.6f) : theme::slot);
        oscButton.setTooltip ("OSC tap: " + (c.oscTap.isNotEmpty() ? c.oscTap : juce::String ("none")) + ", OSC BPM: " + (c.oscBpm.isNotEmpty() ? c.oscBpm : juce::String ("none"))
                              + "\nClick = learn, right click = none. Fixed addresses also work: /stagerack/tap" + (index > 0 ? "/" + juce::String (index) : juce::String())
                              + ", /stagerack/bpm" + (index > 0 ? "/" + juce::String (index) : juce::String()) + " <bpm>, /beat, /bar, /half, /double");

        clockButton.setToggleState (c.midiClock, juce::dontSendNotification);
        const bool audio = c.audioPoint == TempoEngine::pointDeviceInput ? c.audioChannel >= 0 : c.audioRackId >= 0;
        audioBox.setSelectedId (! audio ? 1 : c.audioPoint == TempoEngine::pointDeviceInput ? c.audioChannel + 2 : rackItemId (c.audioRackId, c.audioPoint),
                                juce::dontSendNotification);
        modeBox.setSelectedId (c.audioMode + 1, juce::dontSendNotification);
        modeBox.setEnabled (audio);
        threshold.setEnabled (audio);
        afterTapBox.setSelectedId (c.returnToAudioSec + 1, juce::dontSendNotification);
        if (afterTapBox.getSelectedId() == 0) afterTapBox.setText ("After a tap: back after " + juce::String (c.returnToAudioSec) + " s", juce::dontSendNotification);
        afterTapBox.setEnabled (audio);
        windowBox.setSelectedId (juce::roundToInt (c.audioWindowPct), juce::dontSendNotification);
        if (windowBox.getSelectedId() == 0) windowBox.setText (juce::String::fromUTF8 ("\xc2\xb1") + juce::String (c.audioWindowPct, 1) + "%", juce::dontSendNotification);
        windowBox.setEnabled (audio);

        juce::String send;
        if (c.midiOutId.isNotEmpty() && (c.midiOutKind != TempoEngine::midiNone || c.midiOutClock)) send = "MIDI";
        if (c.oscOut) send << (send.isNotEmpty() ? "+" : "") << "OSC";
        sendButton.setButtonText ("SEND: " + (send.isNotEmpty() ? send : juce::String ("off")));
        sendButton.setColour (juce::TextButton::buttonColourId, send.isNotEmpty() ? tempoColour (index).withAlpha (0.45f) : theme::slot);
        if (! threshold.isMouseButtonDown())
            threshold.setValue (c.audioThresholdDb, juce::dontSendNotification);
        audioOn = audio;
        followMode = c.audioMode == TempoEngine::audioFollow;
    }

    void update()
    {
        bpm.repaint();

        if (++slowTicks % 15 == 0)
        {
            fillAudioInputs();   // racks added / renamed / removed
            const auto c = tempo.getConfig (index);
            audioBox.setSelectedId (! audioOn ? 1 : c.audioPoint == TempoEngine::pointDeviceInput ? c.audioChannel + 2 : rackItemId (c.audioRackId, c.audioPoint),
                                    juce::dontSendNotification);
        }

        opinion = tempo.getAudioOpinion (index);

        const auto l = tempo.readAudioLevel (index);
        level = juce::jmax (l, level * 0.85f);
        if (const auto n = tempo.getOnsetCount (index); n != lastOnsets)
        {
            lastOnsets = n;
            flash = 6;
        }
        else if (flash > 0)
        {
            --flash;
        }

        int racks = 0;
        auto& engine = panel.session.getEngine();
        for (int r = 0; r < engine.getNumRacks(); ++r)
            racks += engine.getRack (r).tempoIndex.load() == index ? 1 : 0;
        numRacks = racks;

        repaint (meterArea);
        repaint (statusArea);
        repaint (racksArea);
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.0f, 2.0f);
        g.setColour (theme::panel);
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour (tempoColour (index));
        g.fillRoundedRectangle (b.removeFromLeft (5.0f), 2.5f);

        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f, true));
        g.drawText (index == 0 ? juce::String ("MASTER") : "TEMPO " + juce::String (index), tagArea, juce::Justification::centredLeft, false);
        g.drawText ("TAP FROM", sourcesArea, juce::Justification::centredLeft, false);

        g.setColour (numRacks > 0 ? theme::text : theme::textDim);
        g.drawText (juce::String (numRacks) + (numRacks == 1 ? " rack" : " racks"), racksArea, juce::Justification::centredRight, false);

        // audio input level + hit flash
        auto m = meterArea.toFloat();
        g.setColour (theme::meterBg);
        g.fillRoundedRectangle (m, 2.0f);
        if (audioOn)
        {
            const auto db = juce::Decibels::gainToDecibels (level, -60.0f);
            const auto frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            g.setColour (flash > 0 ? tempoColour (index) : theme::green.withAlpha (0.7f));
            g.fillRoundedRectangle (m.withTrimmedTop (m.getHeight() * (1.0f - frac)), 2.0f);
        }

        juce::String status;
        juce::Colour sc = theme::textDim;
        if (audioOn && opinion.differs)
        {
            status = "AUDIO " + juce::String (opinion.bpm, 1) + (opinion.takesOverIn >= 0.0 ? " IN " + juce::String (juce::roundToInt (std::ceil (opinion.takesOverIn))) + " s" : " IGNORED");
            sc = theme::amber;
        }
        else if (audioOn && opinion.anchored && opinion.bpm > 0.0)
        {
            status = "TAP + AUDIO";
            sc = theme::green;
        }
        else if (audioOn && followMode)
        {
            const auto conf = tempo.getTrackConfidence (index);
            if (tempo.isTracking (index)) { status = "LOCKED " + juce::String (juce::roundToInt (conf * 100.0f)) + "%"; sc = theme::green; }
            else                          { status = "LISTENING"; sc = theme::amber; }
        }
        else if (tempo.isClockRunning (index))
        {
            status = "CLOCK IN";
            sc = theme::green;
        }

        g.setColour (sc);
        g.setFont (theme::font (10.0f, true));
        g.drawText (status, statusArea, juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 6);
        r.removeFromLeft (2);
        auto top = r.removeFromTop (34);
        r.removeFromTop (6);
        auto bottom = r.removeFromTop (26);

        auto left = top.removeFromLeft (130);
        tagArea = left.removeFromTop (12);
        nameLabel.setBounds (left.withTrimmedLeft (-3));
        top.removeFromLeft (6);
        bpm.setBounds (top.removeFromLeft (128));
        top.removeFromLeft (8);
        tap.setBounds (top.removeFromLeft (84));
        top.removeFromLeft (8);
        halfButton.setBounds (top.removeFromLeft (34).reduced (0, 4));
        top.removeFromLeft (3);
        doubleButton.setBounds (top.removeFromLeft (34).reduced (0, 4));
        top.removeFromLeft (10);
        followBox.setBounds (top.removeFromLeft (130).reduced (0, 4));
        top.removeFromLeft (6);
        tapValueBox.setBounds (top.removeFromLeft (100).reduced (0, 4));
        top.removeFromLeft (6);
        sceneToggle.setBounds (top.removeFromLeft (80).reduced (0, 4));
        top.removeFromLeft (6);
        windowBox.setBounds (top.removeFromLeft (76).reduced (0, 4));
        top.removeFromLeft (6);
        afterTapBox.setBounds (top.removeFromLeft (250).reduced (0, 4));
        removeButton.setBounds (top.removeFromRight (26).reduced (0, 5));
        top.removeFromRight (6);
        racksArea = top;

        sourcesArea = bottom.removeFromLeft (70);
        keyButton.setBounds (bottom.removeFromLeft (110));
        bottom.removeFromLeft (6);
        midiButton.setBounds (bottom.removeFromLeft (140));
        bottom.removeFromLeft (6);
        clockButton.setBounds (bottom.removeFromLeft (92));
        bottom.removeFromLeft (6);
        oscButton.setBounds (bottom.removeFromLeft (150));
        bottom.removeFromLeft (6);
        sendButton.setBounds (bottom.removeFromRight (120));
        bottom.removeFromRight (8);
        bottom.removeFromLeft (8);
        audioBox.setBounds (bottom.removeFromLeft (190));
        bottom.removeFromLeft (6);
        modeBox.setBounds (bottom.removeFromLeft (132));
        bottom.removeFromLeft (6);
        threshold.setBounds (bottom.removeFromLeft (80));
        bottom.removeFromLeft (6);
        meterArea = bottom.removeFromLeft (8);
        bottom.removeFromLeft (8);
        statusArea = bottom;
    }

    TempoPanel& panel;
    TempoEngine& tempo;
    const int index;

private:
    juce::Label nameLabel;
    BpmDisplay bpm;
    TapButton tap;
    juce::TextButton halfButton, doubleButton { "x2" };
    juce::ComboBox followBox, tapValueBox, audioBox, modeBox, afterTapBox, windowBox;
    juce::TextButton sendButton;
    juce::String audioSignature;
    TempoEngine::AudioOpinion opinion;
    int slowTicks = 0;
    juce::ToggleButton sceneToggle;
    LearnButton keyButton, midiButton, oscButton;
    juce::TextButton clockButton { "MIDI CLOCK" }, removeButton;
    juce::Slider threshold;
    juce::StringArray audioNames;
    juce::Rectangle<int> tagArea, sourcesArea, racksArea, meterArea, statusArea;
    float level = 0.0f;
    juce::uint32 lastOnsets = 0;
    int flash = 0, numRacks = 0;
    bool audioOn = false, followMode = false;
};

//==============================================================================
struct RangeOption { double lo, hi; const char* name; };
static const RangeOption rangeOptions[] = {
    { 40.0, 240.0, "40 - 240" }, { 50.0, 200.0, "50 - 200" }, { 60.0, 180.0, "60 - 180" },
    { 70.0, 140.0, "70 - 140" }, { 80.0, 160.0, "80 - 160" }, { 90.0, 180.0, "90 - 180" }, { 100.0, 200.0, "100 - 200" } };
static const double resetOptions[] = { 1.0, 1.5, 2.0, 3.0, 4.0 };
static const int tapsOptions[] = { 2, 3, 4, 6, 8, 12, 16 };

TempoPanel::TempoPanel (Session& s) : session (s)
{
    setWantsKeyboardFocus (true);

    auto label = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (theme::font (11.0f, true));
        l.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (l);
    };

    label (tapsLabel, "Taps averaged");
    label (resetLabel, "New series after");
    label (roundLabel, "Round");
    label (rangeLabel, "Range (BPM)");

    for (auto n : tapsOptions) tapsBox.addItem (juce::String (n), n);
    tapsBox.setTooltip ("How many of the last taps make the tempo (a least-squares fit: every tap counts, a sloppy one moves it little). "
                        "A tap far off the tempo starts a new series by itself.");
    for (int k = 0; k < (int) std::size (resetOptions); ++k) resetBox.addItem (juce::String (resetOptions[k], 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " s", k + 1);
    resetBox.setTooltip ("A pause longer than this starts a new tap series");
    roundBox.addItem ("Off", 1);
    roundBox.addItem ("0.5 BPM", 2);
    roundBox.addItem ("1 BPM", 3);
    roundBox.setTooltip ("Round tapped tempos (MIDI clock is never rounded)");
    for (int k = 0; k < (int) std::size (rangeOptions); ++k) rangeBox.addItem (rangeOptions[k].name, k + 1);
    rangeBox.setTooltip ("Tapped tempos outside are folded by octaves (tapping half notes at 70 gives 140 with 70 - 140)");
    alignToggle.setTooltip ("A tap also puts the beat on the tap (with Tap = Bar: the downbeat), so synced LFOs and "
                            "repeats land with the band. Off: taps only set the tempo.");

    for (auto* c : { &tapsBox, &resetBox, &roundBox, &rangeBox })
    {
        c->onChange = [this] { saveSettings(); };
        addAndMakeVisible (*c);
    }

    alignToggle.onClick = [this] { saveSettings(); };
    addAndMakeVisible (alignToggle);

    helpLabel.setFont (theme::font (11.0f));
    helpLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (helpLabel);

    oscToggle.setButtonText ("OSC in, UDP port");
    oscToggle.setTooltip ("Receive OSC for the tempos (taps, BPM, beat). macOS may ask once to allow incoming connections. "
                          "Saved with the app settings, not in the session.");
    oscToggle.onClick = [this] { applyOsc(); };
    addAndMakeVisible (oscToggle);
    oscPort.setInputRestrictions (5, "0123456789");
    oscPort.setJustification (juce::Justification::centred);
    oscPort.setTooltip ("UDP port StageRack listens on (the sender's \"outgoing\" port), e.g. 8000");
    oscPort.onReturnKey = [this] { applyOsc(); };
    oscPort.onFocusLost = [this] { applyOsc(); };
    addAndMakeVisible (oscPort);
    oscStatus.setFont (theme::font (11.0f));
    addAndMakeVisible (oscStatus);
    {
        const auto& osc = session.getEngine().osc;
        oscToggle.setToggleState (osc.isWanted(), juce::dontSendNotification);
        oscPort.setText (juce::String (osc.getPort()), false);
    }
    updateOscStatus();

    addButton.setTooltip ("Add a tempo (it follows the master until you tap it or give it its own tempo)");
    addButton.onClick = [this] { session.addTempo(); tick(); };
    addAndMakeVisible (addButton);

    loadSettings();
    lookAndFeelChanged();
    rebuildRows();
    startTimerHz (30);
}

TempoPanel::~TempoPanel()
{
    session.getEngine().tempo.learnMidi (-1);
    session.getEngine().tempo.learnOsc (-1, 0);
}

void TempoPanel::applyOsc()
{
    auto& osc = session.getEngine().osc;
    const int port = juce::jlimit (1, 65535, oscPort.getText().getIntValue() > 0 ? oscPort.getText().getIntValue() : OscInput::defaultPort);
    oscPort.setText (juce::String (port), false);

    if (oscToggle.getToggleState() == osc.isWanted() && port == osc.getPort() && (osc.isListening() || ! osc.isWanted()))
        return;

    osc.setEnabled (oscToggle.getToggleState(), port);
    auto& settings = session.getPluginManager().getSettings();
    settings.setValue ("oscIn", oscToggle.getToggleState());
    settings.setValue ("oscPort", port);
    updateOscStatus();
}

void TempoPanel::updateOscStatus()
{
    const auto& osc = session.getEngine().osc;
    juce::String s;
    juce::Colour c = theme::textDim;

    if (! osc.isWanted())
    {
        s = "off";
    }
    else if (! osc.isListening())
    {
        s = "port " + juce::String (osc.getPort()) + " not available (busy?)";
        c = theme::red;
    }
    else
    {
        s = "listening on " + juce::IPAddress::getLocalAddress().toString() + " : " + juce::String (osc.getPort());
        if (const auto n = osc.getMessageCount(); n > 0)
            s << "   " << juce::String (n) << " msg, last " << osc.getLastAddress();
        c = theme::green;
    }

    oscStatus.setText (s, juce::dontSendNotification);
    oscStatus.setColour (juce::Label::textColourId, c);
}

void TempoPanel::rebuildRows()
{
    const int n = session.getEngine().tempo.getNumTempos();
    rows.clear();

    for (int i = 0; i < n; ++i)
        addAndMakeVisible (rows.add (new Row (*this, i)));

    addButton.setEnabled (n < TempoEngine::maxTempos);
    setSize (1330, 50 + n * 86 + 84);   // the window follows (content resizes it)
    resized();
}

void TempoPanel::loadSettings()
{
    const auto st = session.getEngine().tempo.getSettings();
    int tapsId = 8;
    for (auto n : tapsOptions) if (n <= st.maxTaps) tapsId = n;
    tapsBox.setSelectedId (tapsId, juce::dontSendNotification);

    int resetId = 3;
    for (int k = 0; k < (int) std::size (resetOptions); ++k) if (std::abs (resetOptions[k] - st.resetSec) < 0.01) resetId = k + 1;
    resetBox.setSelectedId (resetId, juce::dontSendNotification);
    roundBox.setSelectedId (st.roundStep >= 0.99 ? 3 : (st.roundStep > 0.0 ? 2 : 1), juce::dontSendNotification);

    int rangeId = 1;
    for (int k = 0; k < (int) std::size (rangeOptions); ++k)
        if (std::abs (rangeOptions[k].lo - st.minBpm) < 0.5 && std::abs (rangeOptions[k].hi - st.maxBpm) < 0.5) rangeId = k + 1;
    rangeBox.setSelectedId (rangeId, juce::dontSendNotification);
    alignToggle.setToggleState (st.alignBeat, juce::dontSendNotification);
}

void TempoPanel::saveSettings()
{
    auto& tempo = session.getEngine().tempo;
    auto st = tempo.getSettings();
    st.maxTaps = juce::jmax (2, tapsBox.getSelectedId());
    st.resetSec = resetOptions[juce::jlimit (0, (int) std::size (resetOptions) - 1, resetBox.getSelectedId() - 1)];
    st.roundStep = roundBox.getSelectedId() == 3 ? 1.0 : (roundBox.getSelectedId() == 2 ? 0.5 : 0.0);
    const auto& range = rangeOptions[juce::jlimit (0, (int) std::size (rangeOptions) - 1, rangeBox.getSelectedId() - 1)];
    st.minBpm = range.lo;
    st.maxBpm = range.hi;
    st.alignBeat = alignToggle.getToggleState();
    tempo.setSettings (st);
    session.markDirty();
}

void TempoPanel::tick()
{
    auto& tempo = session.getEngine().tempo;

    if (rows.size() != tempo.getNumTempos())
        rebuildRows();

    if (const auto change = tempo.getChangeCount(); change != lastChange)
    {
        lastChange = change;
        loadSettings();
        for (auto* r : rows) r->refresh();
    }

    for (auto* r : rows)
        r->update();

    if (const auto n = session.getEngine().osc.getMessageCount(); n != lastOscCount || ++oscTicks % 15 == 0)
    {
        lastOscCount = n;
        updateOscStatus();
    }

    // MIDI taps need an enabled input
    static int counter = 0;
    if (++counter % 30 == 1 || helpLabel.getText().isEmpty())
    {
        auto& dm = session.getEngine().getDeviceManager();
        int enabled = 0;
        for (auto& d : juce::MidiInput::getAvailableDevices())
            enabled += dm.isMidiInputDeviceEnabled (d.identifier) ? 1 : 0;

        helpLabel.setText (juce::String ("Racks choose their tempo in the rack menu > Tempo. Their plugins get it as the host tempo (synced delays, LFOs). ")
                               + (enabled == 0 ? "No MIDI input enabled: enable one in SETTINGS for MIDI taps and clock."
                                               : juce::String (enabled) + " MIDI input" + (enabled > 1 ? "s" : "") + " enabled.")
                             + "\nOSC: /stagerack/tap, /stagerack/bpm <bpm>, /stagerack/beat, /bar, /half, /double, /follow <ratio>"
                               "  (master; add /1, /2... for T1, T2...), or learn any address with the OSC button of a tempo.",
                           juce::dontSendNotification);
    }
}

std::unique_ptr<juce::Component> TempoPanel::createSendPanel (Session& s, int index)
{
    return std::make_unique<SendPanel> (s, index);
}

void TempoPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
}

void TempoPanel::lookAndFeelChanged()
{
    for (auto* l : { &tapsLabel, &resetLabel, &roundLabel, &rangeLabel })
        l->setColour (juce::Label::textColourId, theme::textDim);
    helpLabel.setColour (juce::Label::textColourId, theme::textDim);
}

void TempoPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto header = r.removeFromTop (28);

    auto pair = [&header] (juce::Label& l, juce::Component& c, int lw, int cw)
    {
        l.setBounds (header.removeFromLeft (lw));
        header.removeFromLeft (6);
        c.setBounds (header.removeFromLeft (cw));
        header.removeFromLeft (18);
    };

    pair (tapsLabel, tapsBox, 96, 60);
    pair (resetLabel, resetBox, 110, 70);
    pair (roundLabel, roundBox, 46, 90);
    pair (rangeLabel, rangeBox, 84, 100);
    alignToggle.setBounds (header.removeFromLeft (120));

    r.removeFromTop (8);
    helpLabel.setBounds (r.removeFromBottom (42));
    auto bottomRow = r.removeFromBottom (36);
    addButton.setBounds (bottomRow.removeFromLeft (140).reduced (0, 4));
    bottomRow.removeFromLeft (30);
    oscToggle.setBounds (bottomRow.removeFromLeft (130).reduced (0, 4));
    oscPort.setBounds (bottomRow.removeFromLeft (64).reduced (0, 6));
    bottomRow.removeFromLeft (10);
    oscStatus.setBounds (bottomRow);

    for (auto* row : rows)
        row->setBounds (r.removeFromTop (86));
}

bool TempoPanel::keyPressed (const juce::KeyPress& key)
{
    auto& tempo = session.getEngine().tempo;

    if (keyLearn >= 0)
    {
        const auto i = keyLearn;
        keyLearn = -1;

        if (key != juce::KeyPress::escapeKey)
        {
            auto c = tempo.getConfig (i);
            const auto mods = key.getModifiers();
            c.keyCode = (key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey) ? 0
                      : (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown()) ? c.keyCode : key.getKeyCode();
            tempo.setConfig (i, c);
            session.markDirty();
        }

        for (auto* row : rows) row->refresh();
        return true;
    }

    return keys.keyPressed (tempo, key);
}

bool TempoPanel::keyStateChanged (bool)
{
    keys.keyStateChanged();
    return false;
}

//==============================================================================
TempoWindow::TempoWindow (Session& s)
    : juce::DocumentWindow ("Tempo", theme::bg, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new TempoPanel (s), true);
    setResizable (true, false);
    setResizeLimits (1330, 300, 4000, 1400);
    centreWithSize (getWidth(), getHeight());
}

} // namespace stagerack
