#include "AutomixerWindow.h"
#include "Theme.h"

namespace stagerack
{

static constexpr float gainRangeDb = 40.0f;   // gain meter: 0 .. -40 dB
static constexpr float levelRangeDb = 60.0f;  // input meter: 0 .. -60 dBFS

juce::Colour AutomixerPanel::groupColour (int group)
{
    switch (group)
    {
        case 1:  return theme::green;
        case 2:  return theme::accent;
        case 3:  return theme::amber;
        case 4:  return juce::Colour (0xfff26fd1);
        default: return theme::textDim;
    }
}

static juce::String groupName (int group)
{
    return group <= 0 ? juce::String ("Off") : juce::String::charToString ((juce::juce_wchar) ('A' + group - 1));
}

//==============================================================================
class AutomixerPanel::Strip final : public juce::Component
{
public:
    Strip (Session& s, int rackId) : session (s), id (rackId)
    {
        groupBox.addItem ("Off", 1);
        for (int g = 1; g <= Rack::maxAutomixGroups; ++g)
            groupBox.addItem ("Group " + groupName (g), 1 + g);
        groupBox.setTooltip ("Automix group of this rack (Off = not automixed)");
        groupBox.onChange = [this]
        {
            if (auto* r = rack())
            {
                r->automixGroup = groupBox.getSelectedId() - 1;
                session.markDirty();
                repaint();
            }
        };
        addAndMakeVisible (groupBox);

        weight.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        weight.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        weight.setRange (-20.0, 6.0, 0.5);
        weight.setDoubleClickReturnValue (true, 0.0);
        weight.setTooltip ("Weight: bias of this mic in its group (double click = 0 dB)");
        weight.onValueChange = [this]
        {
            if (auto* r = rack())
                r->automixWeightDb = (float) weight.getValue();
            repaint (weightTextArea);
        };
        weight.onDragEnd = [this] { session.markDirty(); };
        addAndMakeVisible (weight);

        muteButton.setClickingTogglesState (true);
        muteButton.setTooltip ("Mute rack (a muted mic leaves the automix at once)");
        muteButton.onClick = [this]
        {
            if (auto* r = rack())
            {
                r->muted = muteButton.getToggleState();
                session.markDirty();
            }
        };
        addAndMakeVisible (muteButton);

        lookAndFeelChanged();
        update();
    }

    int getId() const noexcept { return id; }

    void lookAndFeelChanged() override
    {
        muteButton.setColour (juce::TextButton::buttonOnColourId, theme::red);
        weight.setColour (juce::Slider::rotarySliderFillColourId, theme::green);
    }

    void update()
    {
        auto* r = rack();

        if (r == nullptr)
            return;

        const auto group = r->automixGroup.load();
        const bool active = group > 0;

        // meters: fast up, smooth fall (like the rack meters)
        const auto lvl = active ? r->automixLevelDb.load() : -100.0f;
        levelDb = lvl > levelDb ? lvl : juce::jmax (lvl, levelDb - 1.5f);
        gainDb = active ? r->automixGainDb.load() : 0.0f;

        if (groupBox.getSelectedId() != group + 1)
            groupBox.setSelectedId (group + 1, juce::dontSendNotification);

        if (! weight.isMouseButtonDown() && std::abs ((float) weight.getValue() - r->automixWeightDb.load()) > 0.01f)
            weight.setValue (r->automixWeightDb.load(), juce::dontSendNotification);

        weight.setEnabled (active);
        muteButton.setToggleState (r->muted.load(), juce::dontSendNotification);

        if (name != r->name || colour != r->colour || index != session.getEngine().indexOf (*r))
        {
            name = r->name;
            colour = r->colour;
            index = session.getEngine().indexOf (*r);
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto* r = rack();
        const auto group = r != nullptr ? r->automixGroup.load() : 0;
        const bool active = group > 0;
        const bool enabled = session.getEngine().automixEnabled.load();

        auto b = getLocalBounds().toFloat().reduced (2.0f, 0.0f);
        g.setColour (theme::panel);
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour (colour);
        g.fillRoundedRectangle (b.removeFromTop (5.0f), 2.5f);

        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f, true));
        g.drawText (juce::String (index + 1).paddedLeft ('0', 2), nameArea.withWidth (20.0f), juce::Justification::centredLeft);
        g.setColour (theme::text);
        g.setFont (theme::font (11.5f, true));
        g.drawText (name, nameArea.withTrimmedLeft (20), juce::Justification::centredLeft, true);

        // gain scale, right of the gain meter
        g.setFont (theme::font (8.5f));
        for (int db = 0; db <= (int) gainRangeDb; db += 10)
        {
            const auto y = gainArea.getY() + gainArea.getHeight() * (float) db / gainRangeDb;
            g.setColour (theme::outline);
            g.drawHorizontalLine (juce::roundToInt (y), gainArea.getRight() + 1.0f, gainArea.getRight() + 3.0f);
            g.setColour (theme::textDim);
            g.drawText (db == 0 ? juce::String ("0") : "-" + juce::String (db),
                        juce::Rectangle<float> (gainArea.getRight() + 4.0f, y - 6.0f, 20.0f, 12.0f), juce::Justification::centredLeft);
        }

        // input level (the automixer's detector: after the plugins, before the fader)
        {
            g.setColour (theme::meterBg);
            g.fillRoundedRectangle (levelArea, 2.0f);
            const auto norm = juce::jlimit (0.0f, 1.0f, 1.0f + levelDb / levelRangeDb);

            if (active && norm > 0.0f)
            {
                g.setColour (theme::meterColour (juce::Decibels::decibelsToGain (levelDb)));
                g.fillRoundedRectangle (levelArea.withTrimmedTop (levelArea.getHeight() * (1.0f - norm)), 2.0f);
            }
        }

        // automix gain: full = 0 dB (this mic has the floor), empty = attenuated
        {
            g.setColour (theme::meterBg);
            g.fillRoundedRectangle (gainArea, 2.0f);

            if (active)
            {
                const auto norm = enabled ? juce::jlimit (0.0f, 1.0f, 1.0f + gainDb / gainRangeDb) : 1.0f;
                const auto c = AutomixerPanel::groupColour (group);
                g.setColour (enabled ? c : c.withSaturation (0.1f).withAlpha (0.4f));
                g.fillRoundedRectangle (gainArea.withTrimmedTop (gainArea.getHeight() * (1.0f - norm)), 2.0f);
            }
        }

        g.setColour (theme::textDim);
        g.setFont (theme::font (8.5f, true));
        g.drawText ("IN", levelArea.withY (levelArea.getBottom() + 1.0f).withHeight (12.0f).expanded (6.0f, 0.0f), juce::Justification::centred);
        g.drawText ("GAIN", gainArea.withY (gainArea.getBottom() + 1.0f).withHeight (12.0f).expanded (6.0f, 0.0f), juce::Justification::centred);

        // numbers
        g.setFont (theme::font (13.0f, true));
        g.setColour (active ? (enabled ? theme::text : theme::textDim) : theme::textDim);
        g.drawText (! active ? juce::String::fromUTF8 ("\xe2\x80\x94") : (! enabled ? juce::String ("bypass")
                        : (gainDb <= -59.5f ? juce::String ("-inf") : juce::String (gainDb, 1)) + " dB"),
                    gainTextArea, juce::Justification::centred);

        g.setFont (theme::font (10.0f));
        g.setColour (theme::textDim);
        const auto w = r != nullptr ? r->automixWeightDb.load() : 0.0f;
        g.drawText ("WEIGHT " + juce::String (w > 0.0f ? "+" : "") + juce::String (w, 1), weightTextArea, juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 6).withTrimmedTop (5);
        nameArea = r.removeFromTop (18).toFloat();
        r.removeFromTop (4);
        groupBox.setBounds (r.removeFromTop (22));
        r.removeFromTop (8);

        muteButton.setBounds (r.removeFromBottom (22).withSizeKeepingCentre (40, 22));
        r.removeFromBottom (6);
        weightTextArea = r.removeFromBottom (14);
        weight.setBounds (r.removeFromBottom (48).withSizeKeepingCentre (48, 48));
        r.removeFromBottom (4);
        gainTextArea = r.removeFromBottom (20);
        r.removeFromBottom (14);   // IN / GAIN labels

        auto meters = r.withSizeKeepingCentre (70, r.getHeight()).toFloat();
        levelArea = meters.removeFromLeft (10.0f);
        meters.removeFromLeft (8.0f);
        gainArea = meters.removeFromLeft (28.0f);
    }

private:
    Rack* rack() const { return session.getEngine().findRack (id); }

    Session& session;
    const int id;
    juce::ComboBox groupBox;
    juce::Slider weight;
    juce::TextButton muteButton { "M" };

    juce::String name;
    juce::Colour colour;
    int index = 0;
    float levelDb = -100.0f, gainDb = 0.0f;
    juce::Rectangle<float> nameArea, levelArea, gainArea;
    juce::Rectangle<int> gainTextArea, weightTextArea;
};

//==============================================================================
AutomixerPanel::AutomixerPanel (Session& s) : session (s)
{
    enableButton.setClickingTogglesState (true);
    enableButton.setTooltip ("Automixer on/off for all groups (stored in scenes). Off = every rack at unity gain.");
    enableButton.onClick = [this]
    {
        session.getEngine().automixEnabled = enableButton.getToggleState();
        session.markDirty();
    };
    addAndMakeVisible (enableButton);

    infoLabel.setText ("Gain sharing: the mics of a group share one mic's worth of gain. Latency 0.",
                       juce::dontSendNotification);

    overlapLabel.setText ("When several talk:", juce::dontSendNotification);
    overlapLabel.setFont (theme::font (11.0f));
    overlapLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (overlapLabel);

    overlapBox.addItem ("Share (Dugan)", 1);
    overlapBox.addItem ("Soft", 2);
    overlapBox.addItem ("Open", 3);
    overlapBox.setTooltip ("Share: like Dugan, two equal talkers -3 dB each, room noise of one mic.\n"
                           "Soft: -1.2 dB each. Open: 0 dB each. Pauses and a single talker stay exactly Dugan.");
    overlapBox.onChange = [this]
    {
        session.getEngine().automixOverlap = overlapBox.getSelectedId() - 1;
        session.markDirty();
    };
    addAndMakeVisible (overlapBox);
    infoLabel.setFont (theme::font (11.0f));
    addAndMakeVisible (infoLabel);

    viewport.setViewedComponent (&stripHolder, false);
    viewport.setScrollBarsShown (false, true);
    addAndMakeVisible (viewport);

    lookAndFeelChanged();
    setSize (760, 440);
    tick();
    startTimerHz (30);
}

AutomixerPanel::~AutomixerPanel()
{
    stopTimer();
}

void AutomixerPanel::lookAndFeelChanged()
{
    enableButton.setColour (juce::TextButton::buttonOnColourId, theme::green.withAlpha (0.8f));
    infoLabel.setColour (juce::Label::textColourId, theme::textDim);
    overlapLabel.setColour (juce::Label::textColourId, theme::textDim);
}

void AutomixerPanel::rebuildStrips()
{
    strips.clear();
    stripIds.clear();
    auto& engine = session.getEngine();

    for (int i = 0; i < engine.getNumRacks(); ++i)
    {
        const auto id = engine.getRack (i).getId();
        stripIds.push_back (id);
        stripHolder.addAndMakeVisible (strips.add (new Strip (session, id)));
    }

    resized();
}

void AutomixerPanel::tick()
{
    auto& engine = session.getEngine();
    bool same = (int) stripIds.size() == engine.getNumRacks();

    for (int i = 0; same && i < engine.getNumRacks(); ++i)
        same = stripIds[(size_t) i] == engine.getRack (i).getId();

    if (! same)
        rebuildStrips();

    enableButton.setToggleState (engine.automixEnabled.load(), juce::dontSendNotification);
    enableButton.setButtonText (engine.automixEnabled.load() ? "AUTOMIX ON" : "AUTOMIX OFF");

    if (overlapBox.getSelectedId() != engine.automixOverlap.load() + 1)
        overlapBox.setSelectedId (engine.automixOverlap.load() + 1, juce::dontSendNotification);

    for (auto* s : strips)
        s->update();
}

void AutomixerPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
}

void AutomixerPanel::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto header = r.removeFromTop (28);
    enableButton.setBounds (header.removeFromLeft (120));
    header.removeFromLeft (12);
    overlapBox.setBounds (header.removeFromRight (130).reduced (0, 2));
    overlapLabel.setBounds (header.removeFromRight (120));
    infoLabel.setBounds (header);
    r.removeFromTop (10);

    viewport.setBounds (r);
    const int stripW = 96;
    const int h = r.getHeight() - (strips.size() * stripW > r.getWidth() ? viewport.getScrollBarThickness() : 0);
    stripHolder.setSize (juce::jmax (r.getWidth(), strips.size() * stripW), h);

    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (i * stripW, 0, stripW, h);
}

//==============================================================================
AutomixerWindow::AutomixerWindow (Session& s)
    : juce::DocumentWindow ("Automixer", theme::bg, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new AutomixerPanel (s), true);
    setResizable (true, false);
    setResizeLimits (300, 360, 4000, 1400);
    centreWithSize (getWidth(), getHeight());
}

} // namespace stagerack
