#pragma once

#include <JuceHeader.h>
#include "Theme.h"

namespace stagerack
{

/**
    Click once on a knob / bar to type its value (user request 2026-11-22).

    A plain click (no modifier, no movement) opens a small text field over the control after the
    double-click time, so a double-click still resets and a drag still drags. Return or clicking
    elsewhere applies, Esc cancels. The field lives on the top-level component so it is never
    clipped by a thin rack.
*/
class ValueEntry
{
public:
    using Commit = std::function<void (const juce::String&)>;

    /** Call from mouseDown: a second click cancels the pending field (double-click). */
    void mouseDown() noexcept   { ++serial; }

    /** Call from mouseUp: opens the field if it was a plain click. */
    void mouseUp (juce::Component& c, const juce::MouseEvent& e, std::function<juce::String()> textFn, Commit commitFn)
    {
        if (! isPlainClick (e) || ! c.isEnabled())
            return;

        const auto mySerial = ++serial;
        juce::Component::SafePointer<juce::Component> safe (&c);
        auto* self = this;
        // `this` lives inside `c`: only touched while `safe` says c is alive
        juce::Timer::callAfterDelay (juce::MouseEvent::getDoubleClickTimeout() + 20,
            [safe, self, mySerial, currentText = std::move (textFn), commit = std::move (commitFn)]
            {
                if (safe == nullptr || self->serial != mySerial || juce::Desktop::getInstance().getMainMouseSource().isDragging())
                    return;

                show (*safe, currentText(), commit);
            });
    }

    static bool isPlainClick (const juce::MouseEvent& e)
    {
        return ! e.mods.isAnyModifierKeyDown() && ! e.mods.isPopupMenu() && e.getDistanceFromDragStart() <= 2
               && e.getNumberOfClicks() == 1;
    }

    /** Shows the field over `target` right away. */
    static void show (juce::Component& target, const juce::String& text, Commit commit)
    {
        auto* top = target.getTopLevelComponent();

        if (top == nullptr)
            return;

        auto area = top->getLocalArea (&target, target.getLocalBounds());
        const auto w = juce::jmax (64, juce::jmin (120, area.getWidth()));
        area = juce::Rectangle<int> (w, 24).withCentre (area.getCentre()).constrainedWithin (top->getLocalBounds());

        auto* field = new Field (std::move (commit));
        top->addAndMakeVisible (field);
        field->setBounds (area);
        field->setText (text, false);
        field->selectAll();
        field->grabKeyboardFocus();
    }

private:
    class Field final : public juce::TextEditor
    {
    public:
        explicit Field (Commit c) : commit (std::move (c))
        {
            setJustification (juce::Justification::centred);
            setFont (theme::font (14.0f, true));
            setColour (juce::TextEditor::backgroundColourId, theme::panelLight);
            setColour (juce::TextEditor::outlineColourId, theme::accent);
            setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
            setColour (juce::TextEditor::textColourId, theme::text);
            setSelectAllWhenFocused (true);
            setEscapeAndReturnKeysConsumed (true);
            onReturnKey = [this] { finish (true); };
            onEscapeKey = [this] { finish (false); };
            onFocusLost = [this] { finish (true); };
        }

    private:
        void finish (bool apply)
        {
            if (done)
                return;

            done = true;
            const auto t = getText().trim();

            if (apply && t.isNotEmpty() && commit != nullptr)
                commit (t);

            setVisible (false);
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)]
            {
                delete safe.getComponent();
            });
        }

        Commit commit;
        bool done = false;
    };

    int serial = 0;
};

/** Number typed by the user ("-6", "-6 dB", "+3,5"): decimal comma accepted. */
inline double parseTypedNumber (const juce::String& t)
{
    return t.replaceCharacter (',', '.').retainCharacters ("-+.0123456789").getDoubleValue();
}

} // namespace stagerack
