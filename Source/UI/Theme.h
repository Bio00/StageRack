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
    inline juce::Colour well        { 0xff15171a };   // parameter bars, sunken areas
    inline juce::Colour meterBg     { 0xff0c0d0f };
    inline juce::Colour display     { 0xff0e1013 };   // scene display in the top bar
    inline bool light = false;

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
