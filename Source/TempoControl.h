#pragma once

#include "Style.h"
#include "Text.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <functional>

// Project tempo: the number can be dragged with the mouse (up = faster), double-click types a value,
// and holding "-" / "+" keeps changing it, faster the longer it is held.
class TempoControl : public juce::Component, private juce::Timer
{
public:
    std::function<void()> onValueChange;

    TempoControl()
    {
        label.setJustificationType (juce::Justification::centred);
        label.setEditable (false, true, false);   // double-click edits
        label.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        label.setColour (juce::Label::backgroundColourId, Pal::sunken);
        label.setColour (juce::Label::textColourId, Pal::text);
        label.setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        label.setTooltip ("Темп проекта. Зажмите число и двигайте мышь вверх/вниз (Shift - точнее), двойной клик - ввести значение."_ru);
        label.addMouseListener (this, false);
        label.onTextChange = [this]
        {
            const auto text = label.getText().retainCharacters ("0123456789.,").replaceCharacter (',', '.');
            const double v = text.getDoubleValue();
            if (v > 0.0)
                setValue (v, juce::sendNotification);
            else
                refreshLabel();
        };
        addAndMakeVisible (label);

        for (auto* b : { &minus, &plus })
        {
            b->setTriggeredOnMouseDown (true);
            addAndMakeVisible (*b);
        }
        minus.setButtonText ("-");
        plus.setButtonText ("+");
        minus.setTooltip ("Медленнее. Зажмите, чтобы менять быстрее."_ru);
        plus.setTooltip ("Быстрее. Зажмите, чтобы менять быстрее."_ru);

        minus.onClick = [this] { step (-0.1); };
        plus.onClick = [this] { step (0.1); };
        minus.onStateChange = [this] { holdChanged (minus, -1); };
        plus.onStateChange = [this] { holdChanged (plus, 1); };

        refreshLabel();
    }

    double getValue() const { return value; }
    bool isMouseButtonDown() const { return holdDir != 0 || dragging; }

    void setValue (double v, juce::NotificationType notify)
    {
        v = std::round (juce::jlimit (20.0, 400.0, v) * 10.0) / 10.0;
        if (v == value)
        {
            refreshLabel();
            return;
        }
        value = v;
        refreshLabel();
        if (notify != juce::dontSendNotification && onValueChange)
            onValueChange();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        plus.setBounds (r.removeFromRight (40));
        r.removeFromRight (2);
        minus.setBounds (r.removeFromRight (40));
        r.removeFromRight (2);
        label.setBounds (r);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == &label)
        {
            dragging = true;
            dragStartValue = value;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == &label && dragging && ! label.isBeingEdited())
            setValue (dragStartValue - (double) e.getDistanceFromDragStartY() * (e.mods.isShiftDown() ? 0.02 : 0.2),
                      juce::sendNotification);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == &label)
            dragging = false;
    }

private:
    juce::Label label;
    juce::TextButton minus, plus;
    double value = 120.0, dragStartValue = 120.0;
    bool dragging = false;
    int holdDir = 0;
    juce::int64 holdSince = 0;

    void refreshLabel() { label.setText (juce::String (value, 1) + " BPM", juce::dontSendNotification); }

    void step (double delta) { setValue (value + delta, juce::sendNotification); }

    void holdChanged (juce::Button& b, int dir)
    {
        if (b.isDown() && holdDir == 0)
        {
            holdDir = dir;
            holdSince = juce::Time::currentTimeMillis();
            startTimer (40);
        }
        else if (! b.isDown() && holdDir == dir)
        {
            holdDir = 0;
            stopTimer();
        }
    }

    // after a short pause the value keeps running: slowly first, then faster the longer the button is held
    void timerCallback() override
    {
        const auto held = juce::Time::currentTimeMillis() - holdSince;
        if (held < 400)
            return;
        const double stepSize = held < 1200 ? 0.1 : held < 2500 ? 0.5 : held < 4000 ? 1.0 : 2.0;
        step ((double) holdDir * stepSize);
    }
};
