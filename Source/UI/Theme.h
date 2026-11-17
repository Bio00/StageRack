#pragma once

#include <JuceHeader.h>

namespace stagerack::theme
{
    // Colours are variables so the whole UI can switch between dark and light (setLight()).
    inline juce::Colour bg          { 0xff131518 };
    inline juce::Colour panel       { 0xff1c1f23 };
    inline juce::Colour panelLight  { 0xff262a2f };
    inline juce::Colour slot        { 0xff2d3238 };
    inline juce::Colour slotHover   { 0xff363c43 };
    inline juce::Colour outline     { 0xff3a4048 };
    inline juce::Colour text        { 0xffe8eaed };
    inline juce::Colour textDim     { 0xff8b939d };
    inline juce::Colour accent      { 0xff31c4f3 };
    inline juce::Colour green       { 0xff3ddc84 };
    inline juce::Colour amber       { 0xffffb341 };
    inline juce::Colour red         { 0xffff4d5e };
    inline juce::Colour midiBlue    { 0xff7b6cff };   // MIDI map mode, bank ring
    inline juce::Colour well        { 0xff15171a };   // parameter bars, sunken areas
    inline juce::Colour meterBg     { 0xff0c0d0f };
    inline juce::Colour display     { 0xff0e1013 };   // scene display in the top bar
    inline bool light = false;
    inline bool tintRacks = true;   // whole rack tinted with its colour (View > Tinted Racks)
    inline int tapFlash = 2;        // beat flash of the TAP buttons: 0 off, 1 subtle, 2 medium, 3 strong, 4 full (SETTINGS)

    /** A rack surface (panel, slot) tinted with the rack colour: greyish in dark, pastel in light. */
    inline juce::Colour tinted (juce::Colour base, juce::Colour rackColour, float amount)
    {
        if (! tintRacks)
            return base;

        return base.interpolatedWith (rackColour.withMultipliedSaturation (light ? 0.85f : 0.7f), amount);
    }

    /** Edit group colours, A..H. */
    inline juce::Colour groupColour (int group)
    {
        static const juce::uint32 c[] = { 0xff5b8cff, 0xffff9f43, 0xff2ed573, 0xffd980fa,
                                          0xff1dd1a1, 0xffff6b81, 0xfffeca57, 0xff9aa5b1 };
        return juce::Colour (c[(size_t) juce::jlimit (0, 7, group - 1)]);
    }

    inline juce::String groupLetter (int group)
    {
        return juce::String::charToString ((juce::juce_wchar) ('A' + juce::jlimit (0, 25, group - 1)));
    }

    inline void setLight (bool shouldBeLight)
    {
        light = shouldBeLight;

        if (light)
        {
            bg = juce::Colour (0xffdfe2e6);  panel = juce::Colour (0xffeef0f2);  panelLight = juce::Colour (0xfff8f9fa);
            slot = juce::Colour (0xffffffff); slotHover = juce::Colour (0xfff3f6f9); outline = juce::Colour (0xffb9c0c8);
            text = juce::Colour (0xff1b1f24); textDim = juce::Colour (0xff5d6670); accent = juce::Colour (0xff0a8fc7);
            green = juce::Colour (0xff1e9e57); amber = juce::Colour (0xffd98900); red = juce::Colour (0xffdc3045);
            well = juce::Colour (0xffe4e8ec); meterBg = juce::Colour (0xffcfd4da); display = juce::Colour (0xffffffff);
        }
        else
        {
            bg = juce::Colour (0xff131518);  panel = juce::Colour (0xff1c1f23);  panelLight = juce::Colour (0xff262a2f);
            slot = juce::Colour (0xff2d3238); slotHover = juce::Colour (0xff363c43); outline = juce::Colour (0xff3a4048);
            text = juce::Colour (0xffe8eaed); textDim = juce::Colour (0xff8b939d); accent = juce::Colour (0xff31c4f3);
            green = juce::Colour (0xff3ddc84); amber = juce::Colour (0xffffb341); red = juce::Colour (0xffff4d5e);
            well = juce::Colour (0xff15171a); meterBg = juce::Colour (0xff0c0d0f); display = juce::Colour (0xff0e1013);
        }
    }

    /** Latency classes in the plugin browser: 0 = none, < 5 ms, >= 5 ms, unknown. */
    inline juce::Colour latencyColour (float ms)
    {
        if (ms < 0.0f)   return outline;
        if (ms < 0.01f)  return green;
        if (ms < 5.0f)   return light ? juce::Colour (0xffd4a800) : juce::Colour (0xfff2d43d);
        return light ? juce::Colour (0xff8e959d) : juce::Colour (0xff6f7680);
    }

    inline juce::Font font (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
    }

    inline juce::Colour meterColour (float level)
    {
        return level > 0.89f ? red : (level > 0.5f ? amber : green);
    }

    /** Draws a vertical peak meter, level in linear gain. */
    inline void drawMeter (juce::Graphics& g, juce::Rectangle<float> r, float level)
    {
        g.setColour (meterBg);
        g.fillRoundedRectangle (r, 1.5f);

        const auto db = juce::Decibels::gainToDecibels (level, -60.0f);
        const auto norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);

        if (norm > 0.0f)
        {
            auto fill = r.withTrimmedTop (r.getHeight() * (1.0f - norm));
            g.setColour (meterColour (level));
            g.fillRoundedRectangle (fill, 1.5f);
        }
    }

    class LookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel() { applyColours(); }

        /** Re-reads the theme colours (after setLight()). */
        void applyColours()
        {
            setColourScheme ({ panel, bg, panelLight, outline, text, accent, juce::Colours::black, panelLight, text });

            setColour (juce::ResizableWindow::backgroundColourId, bg);
            setColour (juce::DocumentWindow::backgroundColourId, bg);
            setColour (juce::TextButton::buttonColourId, panelLight);
            setColour (juce::TextButton::buttonOnColourId, accent);
            setColour (juce::TextButton::textColourOffId, text);
            setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            setColour (juce::ComboBox::backgroundColourId, slot);
            setColour (juce::ComboBox::outlineColourId, outline);
            setColour (juce::ComboBox::textColourId, text);
            setColour (juce::ComboBox::arrowColourId, textDim);
            setColour (juce::PopupMenu::backgroundColourId, panelLight);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.35f));
            setColour (juce::PopupMenu::textColourId, text);
            setColour (juce::ListBox::backgroundColourId, panel);
            setColour (juce::TextEditor::backgroundColourId, slot);
            setColour (juce::TextEditor::outlineColourId, outline);
            setColour (juce::TextEditor::focusedOutlineColourId, accent);
            setColour (juce::TextEditor::textColourId, text);
            setColour (juce::Label::textColourId, text);
            setColour (juce::Slider::rotarySliderFillColourId, accent);
            setColour (juce::Slider::rotarySliderOutlineColourId, slot);
            setColour (juce::Slider::thumbColourId, text);
            setColour (juce::Slider::trackColourId, accent);
            setColour (juce::Slider::backgroundColourId, slot);
            setColour (juce::Slider::textBoxTextColourId, text);
            setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::ScrollBar::thumbColourId, outline);
            setColour (juce::CaretComponent::caretColourId, accent);
            setColour (juce::ToggleButton::textColourId, text);
            setColour (juce::ToggleButton::tickColourId, accent);
            setColour (juce::ToggleButton::tickDisabledColourId, outline);
            setColour (juce::ListBox::outlineColourId, outline);
            setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.3f));
            setColour (juce::TextEditor::highlightedTextColourId, text);
            setColour (juce::PopupMenu::highlightedTextColourId, text);
            setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bgColour,
                                   bool highlighted, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (0.5f);
            auto c = bgColour;

            if (down)             c = c.brighter (0.15f);
            else if (highlighted) c = c.brighter (0.07f);

            g.setColour (c);
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (outline.withAlpha (b.getToggleState() ? 0.0f : 0.8f));
            g.drawRoundedRectangle (r, 4.0f, 1.0f);
        }

        void drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&) override
        {
            juce::DropShadow (juce::Colours::black.withAlpha (light ? 0.25f : 0.6f), 10, { 0, 2 }).drawForPath (g, path);
            g.setColour (panel);
            g.fillPath (path);
            g.setColour (outline);
            g.strokePath (path, juce::PathStrokeType (1.0f));
        }

        juce::Font getTextButtonFont (juce::TextButton&, int h) override
        {
            return font (juce::jmin (13.0f, (float) h * 0.55f), true);
        }

        // as JUCE's, but the side margins shrink on very thin buttons (M S B on the thinnest racks)
        void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
        {
            const auto f = getTextButtonFont (b, b.getHeight());
            g.setFont (f);
            g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                           .withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
            const auto indent = juce::jlimit (1, juce::roundToInt (f.getHeight()), (b.getWidth() - 12) / 3);
            const auto yIndent = juce::jmin (4, b.proportionOfHeight (0.3f));
            g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (indent, yIndent), juce::Justification::centred, 2, 0.7f);
        }

        // thin combo boxes (IN / OUT on the thinnest racks): a small arrow, more room for the text
        static bool thinCombo (const juce::ComboBox& box) noexcept  { return box.getWidth() < 72; }

        void drawComboBox (juce::Graphics& g, int width, int height, bool down, int bx, int by, int bw, int bh, juce::ComboBox& box) override
        {
            if (! thinCombo (box))
                return juce::LookAndFeel_V4::drawComboBox (g, width, height, down, bx, by, bw, bh, box);

            const auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat();
            g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
            g.fillRoundedRectangle (bounds, 3.0f);
            g.setColour (box.findColour (juce::ComboBox::outlineColourId));
            g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);

            const auto zone = juce::Rectangle<float> ((float) width - 9.0f, 0.0f, 6.0f, (float) height);
            juce::Path p;
            p.startNewSubPath (zone.getX(), zone.getCentreY() - 1.5f);
            p.lineTo (zone.getCentreX(), zone.getCentreY() + 1.5f);
            p.lineTo (zone.getRight(), zone.getCentreY() - 1.5f);
            g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 0.9f : 0.2f));
            g.strokePath (p, juce::PathStrokeType (1.5f));
        }

        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            if (! thinCombo (box))
                return juce::LookAndFeel_V4::positionComboBoxText (box, label);

            label.setBounds (0, 1, box.getWidth() - 9, box.getHeight() - 2);
            label.setBorderSize ({ 1, 3, 1, 1 });
            label.setFont (getComboBoxFont (box).withHeight (juce::jmin (12.0f, getComboBoxFont (box).getHeight())));
            label.setMinimumHorizontalScale (0.75f);
        }

        // vertical faders (big input / output sections of a rack): groove, fill in the rack
        // colour, 0 dB mark, console-style cap
        void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float sliderPos, float minPos, float maxPos,
                               juce::Slider::SliderStyle style, juce::Slider& s) override
        {
            if (style != juce::Slider::LinearVertical)
                return juce::LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, sliderPos, minPos, maxPos, style, s);

            const auto cx = (float) x + (float) w * 0.5f;
            const auto groove = juce::Rectangle<float> (cx - 2.0f, (float) y, 4.0f, (float) h);
            g.setColour (light ? outline : slot);
            g.fillRoundedRectangle (groove, 2.0f);
            g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId));
            g.fillRoundedRectangle (groove.withTop (sliderPos), 2.0f);

            if (s.getMinimum() < 0.0 && s.getMaximum() > 0.0)
            {
                const auto zero = (float) s.getPositionOfValue (0.0);
                g.setColour (textDim);
                g.drawHorizontalLine (juce::roundToInt (zero), cx - 11.0f, cx - 4.0f);
                g.drawHorizontalLine (juce::roundToInt (zero), cx + 4.0f, cx + 11.0f);
            }

            const auto capW = juce::jlimit (16.0f, 34.0f, (float) w - 4.0f);
            const auto cap = juce::Rectangle<float> (capW, 18.0f).withCentre ({ cx, sliderPos });
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (cap.translated (0.0f, 1.5f), 3.0f);
            g.setColour (light ? juce::Colour (0xfff4f5f7) : juce::Colour (0xff4a5058));
            g.fillRoundedRectangle (cap, 3.0f);
            g.setColour (outline);
            g.drawRoundedRectangle (cap.reduced (0.5f), 3.0f, 1.0f);
            g.setColour (text);
            g.fillRect (cap.withSizeKeepingCentre (capW - 6.0f, 2.0f));
        }

        int getSliderThumbRadius (juce::Slider& s) override
        {
            return s.getSliderStyle() == juce::Slider::LinearVertical ? 9 : juce::LookAndFeel_V4::getSliderThumbRadius (s);
        }

        void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider& s) override
        {
            auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
            const auto radius = juce::jmin (r.getWidth(), r.getHeight()) / 2.0f;
            const auto centre = r.getCentre();
            const auto angle = startAngle + pos * (endAngle - startAngle);

            juce::Path track, value;
            track.addCentredArc (centre.x, centre.y, radius, radius, 0, startAngle, endAngle, true);
            value.addCentredArc (centre.x, centre.y, radius, radius, 0, startAngle, angle, true);

            g.setColour (light ? outline : slot);   // white track would vanish on the light panel
            g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId));
            g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            g.setColour (text);
            const auto thumb = centre.getPointOnCircumference (radius - 5.0f, angle);
            g.drawLine ({ centre.getPointOnCircumference (radius * 0.2f, angle), thumb }, 2.0f);
        }
    };
}
