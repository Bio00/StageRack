#include "ChangelogWindow.h"
#include "Theme.h"

namespace stagerack
{

static std::unique_ptr<ChangelogWindow>& instance()
{
    static std::unique_ptr<ChangelogWindow> w;
    return w;
}

static std::function<void()> pendingOnClose;

//==============================================================================
/** Read-only text with a few markdown styles, re-rendered when the theme changes. */
class ChangelogWindow::Content final : public juce::Component
{
public:
    Content()
    {
        text.setMultiLine (true, true);
        text.setReadOnly (true);
        text.setCaretVisible (false);
        text.setScrollbarsShown (true);
        text.setPopupMenuEnabled (true);   // copy
        text.setIndents (18, 14);
        addAndMakeVisible (text);
        setSize (620, 640);
    }

    void setMarkdown (const juce::String& md)
    {
        markdown = md;
        render();
    }

    void resized() override                { text.setBounds (getLocalBounds()); }
    void lookAndFeelChanged() override     { render(); }

private:
    void put (const juce::String& s, const juce::Font& f, juce::Colour c)
    {
        text.setFont (f);
        text.setColour (juce::TextEditor::textColourId, c);
        text.insertTextAtCaret (s);
    }

    /** One line with **bold** spans; `code` and single *emphasis* marks are dropped. */
    void putInline (juce::String s, float size, juce::Colour c, bool boldBase = false)
    {
        s = s.replace ("`", "");
        bool bold = boldBase;

        for (const auto& part : juce::StringArray::fromTokens (s.replace ("**", "\x01"), "\x01", ""))
        {
            put (part.replace ("*", ""), theme::font (size, bold), c);
            bold = ! bold;
        }

        put ("\n", theme::font (size), c);
    }

    void render()
    {
        text.setColour (juce::TextEditor::backgroundColourId, theme::bg);
        text.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        text.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        text.setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.3f));
        text.clear();

        bool first = true;

        for (auto line : juce::StringArray::fromLines (markdown.replace ("\r", "")))
        {
            line = line.trimEnd();

            if (line.startsWith ("## "))
            {
                if (! first)
                    put ("\n", theme::font (12.0f), theme::text);
                putInline (line.substring (3), 21.0f, theme::accent, true);
                put ("\n", theme::font (5.0f), theme::text);
            }
            else if (line.startsWith ("### "))
            {
                put ("\n", theme::font (6.0f), theme::text);
                putInline (line.substring (4), 15.5f, theme::text, true);
            }
            else if (line.startsWith ("# "))
            {
                putInline (line.substring (2), 24.0f, theme::text, true);
            }
            else if (line.startsWith ("- ") || line.startsWith ("* "))
            {
                put (juce::String::fromUTF8 ("  \xe2\x80\xa2  "), theme::font (14.0f, true), theme::accent);
                putInline (line.substring (2), 14.0f, theme::text);
                put ("\n", theme::font (4.0f), theme::text);
            }
            else if (line.startsWith ("|"))
            {
                // table: skip the |---| row, cells separated by wide gaps
                if (line.containsOnly ("|-: "))
                    continue;

                auto cells = juce::StringArray::fromTokens (line.trimCharactersAtStart ("|").trimCharactersAtEnd ("|"), "|", "");
                cells.trim();
                cells.removeEmptyStrings();
                putInline ("  " + cells.joinIntoString (juce::String::fromUTF8 ("   \xc2\xb7   ")), 13.0f, theme::textDim);
            }
            else if (line.isEmpty())
            {
                put ("\n", theme::font (6.0f), theme::text);
            }
            else
            {
                putInline (line, 14.0f, theme::text);
            }

            first = false;
        }

        text.setCaretPosition (0);
        text.scrollEditorToPositionCaret (0, 0);
    }

    juce::TextEditor text;
    juce::String markdown;
};

//==============================================================================
ChangelogWindow::ChangelogWindow()
    : juce::DocumentWindow ("What's New", theme::bg, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new Content(), true);
    setResizable (true, false);
    setResizeLimits (360, 300, 1400, 2000);
    centreWithSize (getWidth(), getHeight());
}

void ChangelogWindow::show (const juce::String& title, const juce::String& markdown)
{
    auto& w = instance();

    if (w == nullptr)
        w.reset (new ChangelogWindow());

    w->onClose = std::exchange (pendingOnClose, nullptr);
    w->setName (title);
    static_cast<Content*> (w->getContentComponent())->setMarkdown (markdown);
    w->setVisible (true);
    w->toFront (true);
}

void ChangelogWindow::setOnClose (std::function<void()> f)
{
    pendingOnClose = std::move (f);
}

void ChangelogWindow::closeButtonPressed()
{
    setVisible (false);

    if (auto f = std::exchange (onClose, nullptr))
        juce::MessageManager::callAsync (std::move (f));
}

juce::Image ChangelogWindow::render (const juce::String& markdown, int width, int height)
{
    Content c;
    c.setSize (width, height);
    c.setMarkdown (markdown);
    juce::Image img (juce::Image::ARGB, width, height, true);
    juce::Graphics g (img);
    g.fillAll (theme::bg);
    c.paintEntireComponent (g, true);
    return img;
}

void ChangelogWindow::closeAll()
{
    pendingOnClose = nullptr;
    instance() = nullptr;
}

} // namespace stagerack
