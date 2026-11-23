#include "AudioDevicePanel.h"
#include "Theme.h"

namespace stagerack
{

static const juce::String noDevice = "<< none >>";

//==============================================================================
int AudioDevicePanel::CheckGrid::heightFor (int width)
{
    columns = juce::jmax (1, width / 112);
    const int rows = (names.size() + columns - 1) / columns;
    return juce::jmax (rowH, rows * rowH);
}

int AudioDevicePanel::CheckGrid::indexAt (juce::Point<int> p) const
{
    if (! getLocalBounds().contains (p))
        return -1;

    const int colW = juce::jmax (1, getWidth() / columns);
    const int i = (p.y / rowH) * columns + juce::jmin (columns - 1, p.x / colW);
    return i < names.size() ? i : -1;
}

void AudioDevicePanel::CheckGrid::paint (juce::Graphics& g)
{
    const int colW = juce::jmax (1, getWidth() / columns);

    if (names.isEmpty())
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawText ("(none)", getLocalBounds(), juce::Justification::centredLeft);
        return;
    }

    for (int i = 0; i < names.size(); ++i)
    {
        auto r = juce::Rectangle<int> ((i % columns) * colW, (i / columns) * rowH, colW, rowH).reduced (2, 2);
        const bool isOn = on[i];
        g.setColour (isOn ? theme::accent.withAlpha (0.25f) : theme::slot);
        g.fillRoundedRectangle (r.toFloat(), 3.0f);

        auto box = r.removeFromLeft (r.getHeight()).reduced (3).toFloat();
        g.setColour (isOn ? theme::accent : theme::outline);
        g.fillRoundedRectangle (box, 2.0f);

        if (isOn)
        {
            juce::Path tick;
            tick.startNewSubPath (box.getX() + 2.5f, box.getCentreY());
            tick.lineTo (box.getCentreX() - 0.5f, box.getBottom() - 3.0f);
            tick.lineTo (box.getRight() - 2.5f, box.getY() + 3.0f);
            g.setColour (juce::Colours::black);
            g.strokePath (tick, juce::PathStrokeType (1.6f));
        }

        g.setColour (isOn ? theme::text : theme::textDim);
        g.setFont (theme::font (11.5f, isOn));
        g.drawFittedText (names[i], r.withTrimmedLeft (4), juce::Justification::centredLeft, 1, 0.8f);
    }
}

void AudioDevicePanel::CheckGrid::mouseDown (const juce::MouseEvent& e)
{
    lastIndex = indexAt (e.getPosition());

    if (lastIndex >= 0)
    {
        paintValue = ! on[lastIndex];
        on.setBit (lastIndex, paintValue);
        if (onToggle) onToggle (lastIndex, paintValue);
        repaint();
    }
}

void AudioDevicePanel::CheckGrid::mouseDrag (const juce::MouseEvent& e)
{
    // dragging over the cells sets them all like the first one (on or off)
    const int i = indexAt (e.getPosition());

    if (i >= 0 && i != lastIndex)
    {
        lastIndex = i;
        if (on[i] != paintValue)
        {
            on.setBit (i, paintValue);
            if (onToggle) onToggle (i, paintValue);
            repaint();
        }
    }
}

//==============================================================================
AudioDevicePanel::AudioDevicePanel (juce::AudioDeviceManager& m) : dm (m)
{
    auto label = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (theme::font (12.5f, true));
        addAndMakeVisible (l);
    };

    label (inLabel, "Input device");
    label (outLabel, "Output device");
    label (rateLabel, "Sample rate");
    label (bufferLabel, "Buffer size");
    label (inChLabel, "Input channels");
    label (outChLabel, "Output channels");
    label (midiLabel, "MIDI inputs");

    for (auto* b : { &inBox, &outBox, &rateBox, &bufferBox })
        addAndMakeVisible (*b);

    inBox.onChange = [this]
    {
        const auto t = inBox.getText();
        staged.inputDeviceName = t == noDevice ? juce::String() : t;
        staged.inputChannels.clear();
        readCapabilities();
        staged.inputChannels.setRange (0, inNames.size(), true);   // a new device: all its channels (user request)
        refreshControls();
    };
    outBox.onChange = [this]
    {
        const auto t = outBox.getText();
        staged.outputDeviceName = t == noDevice ? juce::String() : t;
        staged.outputChannels.clear();
        readCapabilities();
        staged.outputChannels.setRange (0, outNames.size(), true);
        refreshControls();
    };
    rateBox.onChange = [this]
    {
        if (const auto i = rateBox.getSelectedItemIndex(); juce::isPositiveAndBelow (i, rates.size()))
            staged.sampleRate = rates[i];
        refreshControls();
    };
    bufferBox.onChange = [this]
    {
        if (const auto i = bufferBox.getSelectedItemIndex(); juce::isPositiveAndBelow (i, buffers.size()))
            staged.bufferSize = buffers[i];
        refreshControls();
    };

    inGrid.onToggle  = [this] (int ch, bool on) { staged.inputChannels.setBit (ch, on);  refreshControls(); };
    outGrid.onToggle = [this] (int ch, bool on) { staged.outputChannels.setBit (ch, on); refreshControls(); };
    midiGrid.onToggle = [this] (int i, bool on)
    {
        if (juce::isPositiveAndBelow (i, midiDevices.size()))
            dm.setMidiInputDeviceEnabled (midiDevices[i].identifier, on);   // at once: no audio reopen
    };
    for (auto* v : { &inView, &outView })
    {
        v->setScrollBarsShown (true, false);
        v->setScrollBarThickness (8);
        addAndMakeVisible (*v);
    }
    inView.setViewedComponent (&inGrid, false);
    outView.setViewedComponent (&outGrid, false);
    addAndMakeVisible (midiGrid);

    inAll.onClick   = [this] { staged.inputChannels.setRange (0, inNames.size(), true);   refreshControls(); };
    inNone.onClick  = [this] { staged.inputChannels.clear();                              refreshControls(); };
    outAll.onClick  = [this] { staged.outputChannels.setRange (0, outNames.size(), true); refreshControls(); };
    outNone.onClick = [this] { staged.outputChannels.clear();                             refreshControls(); };
    for (auto* b : { &inAll, &inNone, &outAll, &outNone })
        addAndMakeVisible (*b);

    applyButton.onClick = [this] { apply(); };
    revertButton.onClick = [this] { loadFromDevice(); };
    applyButton.setTooltip ("Opens the device with these settings (once)");
    revertButton.setTooltip ("Back to what is open now");
    addAndMakeVisible (applyButton);
    addAndMakeVisible (revertButton);

    status.setFont (theme::font (12.0f));
    status.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (status);
    hint.setFont (theme::font (11.5f));
    hint.setText ("Changes are applied together with Apply: the device is reopened only once.", juce::dontSendNotification);
    addAndMakeVisible (hint);

    lookAndFeelChanged();
    dm.addChangeListener (this);
    loadFromDevice();
}

AudioDevicePanel::~AudioDevicePanel()
{
    dm.removeChangeListener (this);
}

void AudioDevicePanel::lookAndFeelChanged()
{
    for (auto* l : { &inLabel, &outLabel, &rateLabel, &bufferLabel, &inChLabel, &outChLabel, &midiLabel })
        l->setColour (juce::Label::textColourId, theme::text);
    hint.setColour (juce::Label::textColourId, theme::textDim);
    applyButton.setColour (juce::TextButton::buttonColourId, theme::accent.withAlpha (0.35f));
    updateStatus();
}

juce::AudioIODeviceType* AudioDevicePanel::type() const
{
    if (auto* t = dm.getCurrentDeviceTypeObject())
        return t;

    auto& types = dm.getAvailableDeviceTypes();
    return types.isEmpty() ? nullptr : types.getFirst();
}

void AudioDevicePanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // the device changed from elsewhere (reconnected, the guard, Apply): follow it unless the user
    // is in the middle of choosing something
    if (! isDirty() || error.isNotEmpty())
        loadFromDevice();
    else
        updateStatus();
}

void AudioDevicePanel::loadFromDevice()
{
    if (auto* t = type())
        t->scanForDevices();   // fresh device names (only here, not at every choice)

    staged = dm.getAudioDeviceSetup();
    error.clear();
    readCapabilities();
    refreshControls();
}

void AudioDevicePanel::readCapabilities()
{
    rates.clear();
    buffers.clear();
    inNames.clear();
    outNames.clear();

    auto* t = type();
    if (t == nullptr)
        return;

    auto* current = dm.getCurrentAudioDevice();
    const auto cur = dm.getAudioDeviceSetup();
    std::unique_ptr<juce::AudioIODevice> probe;
    juce::AudioIODevice* d = nullptr;

    if (current != nullptr && cur.inputDeviceName == staged.inputDeviceName && cur.outputDeviceName == staged.outputDeviceName)
        d = current;   // what is open now: no need to create anything
    else if (staged.inputDeviceName.isNotEmpty() || staged.outputDeviceName.isNotEmpty())
    {
        probe.reset (t->createDevice (staged.outputDeviceName, staged.inputDeviceName));   // created, not opened
        d = probe.get();
    }

    if (d == nullptr)
        return;

    rates = d->getAvailableSampleRates();
    buffers = d->getAvailableBufferSizes();
    inNames = staged.inputDeviceName.isNotEmpty() ? d->getInputChannelNames() : juce::StringArray();
    outNames = staged.outputDeviceName.isNotEmpty() ? d->getOutputChannelNames() : juce::StringArray();

    if (! rates.contains (staged.sampleRate) && ! rates.isEmpty())
        staged.sampleRate = rates.contains (48000.0) ? 48000.0 : rates.getFirst();
    if (! buffers.contains (staged.bufferSize) && ! buffers.isEmpty())
    {
        int best = buffers.getFirst();
        for (auto b : buffers)
            if (std::abs (b - staged.bufferSize) < std::abs (best - staged.bufferSize))
                best = b;
        staged.bufferSize = best;
    }
}

void AudioDevicePanel::refreshControls()
{
    auto* t = type();
    const auto ins = t != nullptr ? t->getDeviceNames (true) : juce::StringArray();
    const auto outs = t != nullptr ? t->getDeviceNames (false) : juce::StringArray();

    auto fillDevices = [] (juce::ComboBox& box, const juce::StringArray& names, const juce::String& selected)
    {
        box.clear (juce::dontSendNotification);
        box.addItem (noDevice, 1);
        box.addItemList (names, 2);
        const auto i = names.indexOf (selected);
        box.setSelectedId (i >= 0 ? i + 2 : 1, juce::dontSendNotification);
    };
    fillDevices (inBox, ins, staged.inputDeviceName);
    fillDevices (outBox, outs, staged.outputDeviceName);

    rateBox.clear (juce::dontSendNotification);
    for (int i = 0; i < rates.size(); ++i)
        rateBox.addItem (juce::String (rates[i], 0) + " Hz", i + 1);
    rateBox.setSelectedItemIndex (rates.indexOf (staged.sampleRate), juce::dontSendNotification);

    bufferBox.clear (juce::dontSendNotification);
    const auto sr = staged.sampleRate > 0 ? staged.sampleRate : 48000.0;
    for (int i = 0; i < buffers.size(); ++i)
        bufferBox.addItem (juce::String (buffers[i]) + " samples (" + juce::String (1000.0 * buffers[i] / sr, 1) + " ms)", i + 1);
    bufferBox.setSelectedItemIndex (buffers.indexOf (staged.bufferSize), juce::dontSendNotification);

    inGrid.names = inNames;
    inGrid.on = staged.inputChannels;
    outGrid.names = outNames;
    outGrid.on = staged.outputChannels;

    midiDevices = juce::MidiInput::getAvailableDevices();
    midiGrid.names.clear();
    midiGrid.on.clear();
    for (int i = 0; i < midiDevices.size(); ++i)
    {
        midiGrid.names.add (midiDevices[i].name);
        midiGrid.on.setBit (i, dm.isMidiInputDeviceEnabled (midiDevices[i].identifier));
    }

    inChLabel.setText ("Input channels  " + juce::String (staged.inputChannels.countNumberOfSetBits()) + "/" + juce::String (inNames.size()), juce::dontSendNotification);
    outChLabel.setText ("Output channels  " + juce::String (staged.outputChannels.countNumberOfSetBits()) + "/" + juce::String (outNames.size()), juce::dontSendNotification);
    inGrid.repaint();
    outGrid.repaint();
    midiGrid.repaint();
    updateStatus();

    if (getWidth() > 0)
    {
        const auto h = layoutFor (getWidth());
        if (h != lastHeight)
        {
            lastHeight = h;
            if (onHeightChanged) onHeightChanged();
        }
    }
}

bool AudioDevicePanel::isDirty() const
{
    const auto cur = dm.getAudioDeviceSetup();
    const auto inCount = juce::jmax (1, inNames.size()), outCount = juce::jmax (1, outNames.size());
    auto bits = [] (const juce::BigInteger& b, int n) { auto c = b; c.setRange (n, juce::jmax (0, c.getHighestBit() + 1 - n), false); return c; };

    return cur.inputDeviceName != staged.inputDeviceName || cur.outputDeviceName != staged.outputDeviceName
        || ! juce::approximatelyEqual (cur.sampleRate, staged.sampleRate) || cur.bufferSize != staged.bufferSize
        || bits (cur.inputChannels, inCount) != bits (staged.inputChannels, inCount)
        || bits (cur.outputChannels, outCount) != bits (staged.outputChannels, outCount);
}

void AudioDevicePanel::updateStatus()
{
    const bool dirty = isDirty();
    applyButton.setEnabled (dirty);
    revertButton.setEnabled (dirty);

    juce::String s;
    auto colour = theme::textDim;

    if (error.isNotEmpty())
    {
        s = "Could not open: " + error;
        colour = theme::red;
    }
    else if (dirty)
    {
        s = "Not applied yet: press Apply.";
        colour = theme::amber;
    }
    else if (auto* d = dm.getCurrentAudioDevice())
    {
        const auto rt = d->getInputLatencyInSamples() + d->getOutputLatencyInSamples() + d->getCurrentBufferSizeSamples();
        s << "Open: " << d->getName() << "  \xc2\xb7  " << d->getActiveInputChannels().countNumberOfSetBits() << " in / "
          << d->getActiveOutputChannels().countNumberOfSetBits() << " out  \xc2\xb7  "
          << juce::String (d->getCurrentSampleRate() / 1000.0, 1) << " kHz  \xc2\xb7  " << d->getCurrentBufferSizeSamples() << " smp  \xc2\xb7  "
          << juce::String (1000.0 * rt / juce::jmax (1.0, d->getCurrentSampleRate()), 1) << " ms round trip";
        s = juce::String::fromUTF8 (s.toRawUTF8());
        colour = theme::green;
    }
    else
    {
        s = "No device open";
        colour = theme::amber;
    }

    status.setText (s, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, colour);
}

void AudioDevicePanel::apply()
{
    auto s = staged;
    s.useDefaultInputChannels = false;
    s.useDefaultOutputChannels = false;

    applyButton.setEnabled (false);
    status.setText ("Opening the device...", juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, theme::textDim);

    // let the message show before the (possibly slow) reopen
    juce::Timer::callAfterDelay (40, [safe = juce::Component::SafePointer<AudioDevicePanel> (this), s]
    {
        if (safe == nullptr)
            return;

        safe->error = safe->dm.setAudioDeviceSetup (s, true);
        // the change listener reloads from the device when it worked; on error the choice stays
        safe->readCapabilities();
        safe->refreshControls();
    });
}

//==============================================================================
int AudioDevicePanel::layoutFor (int w)
{
    int y = 4;
    auto row = [&] (int h) { auto r = juce::Rectangle<int> (0, y, w, h); y += h + 6; return r; };
    const int labelW = 150;

    auto line = [&] (juce::Label& l, juce::ComboBox& b)
    {
        auto r = row (26);
        l.setBounds (r.removeFromLeft (labelW));
        b.setBounds (r.removeFromLeft (juce::jmin (r.getWidth(), 360)));
    };

    line (inLabel, inBox);
    line (outLabel, outBox);
    line (rateLabel, rateBox);
    line (bufferLabel, bufferBox);

    auto buttons = row (30);
    buttons.removeFromLeft (labelW);
    applyButton.setBounds (buttons.removeFromLeft (110));
    buttons.removeFromLeft (8);
    revertButton.setBounds (buttons.removeFromLeft (90));
    status.setBounds (row (34));
    hint.setBounds (row (18));
    y += 6;

    // inputs and outputs side by side (one column on a narrow page); a long list scrolls inside
    // its own box, so Apply and the output channels never need a long scroll of the page
    const bool sideBySide = w >= 520;
    const int colW = sideBySide ? (w - 16) / 2 : w;
    constexpr int maxGridH = 8 * CheckGrid::rowH;

    auto channels = [&] (juce::Label& l, juce::Button& all, juce::Button& none, CheckGrid& g, juce::Viewport& v, int x, int top)
    {
        auto head = juce::Rectangle<int> (x, top, colW, 22);
        none.setBounds (head.removeFromRight (52).reduced (0, 1));
        head.removeFromRight (4);
        all.setBounds (head.removeFromRight (44).reduced (0, 1));
        l.setBounds (head);

        int full = g.heightFor (colW);
        if (full > maxGridH)
            full = g.heightFor (colW - 10);   // room for the scroll bar

        const int shown = juce::jmin (full, maxGridH);
        g.setSize (full > shown ? colW - 10 : colW, full);
        v.setBounds (x, top + 24, colW, shown);
        return top + 24 + shown;
    };

    if (sideBySide)
    {
        const auto bottom = juce::jmax (channels (inChLabel, inAll, inNone, inGrid, inView, 0, y),
                                        channels (outChLabel, outAll, outNone, outGrid, outView, colW + 16, y));
        y = bottom + 14;
    }
    else
    {
        y = channels (inChLabel, inAll, inNone, inGrid, inView, 0, y) + 12;
        y = channels (outChLabel, outAll, outNone, outGrid, outView, 0, y) + 14;
    }

    midiLabel.setBounds (0, y, w, 22);
    y += 24;
    const auto mh = midiGrid.heightFor (w);
    midiGrid.setBounds (0, y, w, mh);
    y += mh + 12;

    setSize (w, y);
    return y;
}

void AudioDevicePanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
}

} // namespace stagerack
