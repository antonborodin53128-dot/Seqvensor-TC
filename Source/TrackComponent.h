#pragma once

#include "Engine.h"
#include "Timeline.h"

// Toggle button used for Mute / Record: bright red when on, pale red when off.
class StateButton : public juce::Button
{
public:
    enum class Style { Text, Dot };

    StateButton (const juce::String& text, Style styleToUse) : juce::Button (text), style (styleToUse)
    {
        setClickingTogglesState (true);
    }

    void setColours (juce::Colour on, juce::Colour off, juce::Colour onEdge, juce::Colour offEdge, juce::Colour offText)
    {
        onColour = on; offColour = off; onEdgeColour = onEdge; offEdgeColour = offEdge; offTextColour = offText;
        repaint();
    }

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        const bool on = getToggleState();
        auto r = getLocalBounds().toFloat().reduced (1.0f);

        g.setColour (on ? onColour : offColour);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (on ? onEdgeColour : (over ? offEdgeColour.brighter (0.3f) : offEdgeColour));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);

        if (style == Style::Text)
        {
            g.setColour (on ? juce::Colours::white : offTextColour);
            g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred);
        }
        else
        {
            const float d = juce::jmin (r.getHeight() - 10.0f, 18.0f);
            auto dot = juce::Rectangle<float> (d, d).withCentre (r.getCentre());
            g.setColour (on ? juce::Colours::white : juce::Colour (0xff9a5a60));
            g.fillEllipse (dot);
        }
    }

private:
    Style style;
    juce::Colour onColour { 0xffff2b38 }, offColour { 0xff4d2a2e }, onEdgeColour { 0xffffa0a6 },
                 offEdgeColour { 0xff6a3a40 }, offTextColour { 0xffc58a90 };
};

// Small drawn icon button (no font glyphs, so nothing can turn into "hieroglyphs").
class IconButton : public juce::Button
{
public:
    enum class Icon { Play, Pause, Stop, Close, Gear, Menu, Arrow };

    explicit IconButton (Icon i) : juce::Button ({}), icon (i) {}

    void setIcon (Icon i) { icon = i; repaint(); }
    void setActive (bool a) { if (a != active) { active = a; repaint(); } }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (active ? juce::Colour (0xff1f8f5f)
                            : down ? juce::Colour (0xff4a5059) : over ? juce::Colour (0xff3a4048) : juce::Colour (0xff2c3138));
        g.fillRoundedRectangle (r, 5.0f);

        g.setColour (juce::Colour (0xffe6e9ed));
        auto c = r.getCentre();
        const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.28f;

        switch (icon)
        {
            case Icon::Play:
            {
                juce::Path p;
                p.addTriangle (c.x - s * 0.7f, c.y - s, c.x - s * 0.7f, c.y + s, c.x + s * 1.1f, c.y);
                g.fillPath (p);
                break;
            }
            case Icon::Pause:
                g.fillRect (juce::Rectangle<float> (s * 0.6f, s * 2.0f).withCentre ({ c.x - s * 0.6f, c.y }));
                g.fillRect (juce::Rectangle<float> (s * 0.6f, s * 2.0f).withCentre ({ c.x + s * 0.6f, c.y }));
                break;
            case Icon::Stop:
                g.fillRect (juce::Rectangle<float> (s * 1.8f, s * 1.8f).withCentre (c));
                break;
            case Icon::Close:
                g.drawLine (c.x - s, c.y - s, c.x + s, c.y + s, 2.0f);
                g.drawLine (c.x - s, c.y + s, c.x + s, c.y - s, 2.0f);
                break;
            case Icon::Arrow:
            {
                juce::Path p;
                p.addTriangle (c.x - s * 0.9f, c.y - s * 0.45f, c.x + s * 0.9f, c.y - s * 0.45f, c.x, c.y + s * 0.6f);
                g.fillPath (p);
                break;
            }
            case Icon::Menu:
                for (int i = -1; i <= 1; ++i)
                    g.fillRoundedRectangle (c.x - s * 1.3f, c.y + (float) i * s * 0.9f - 1.0f, s * 2.6f, 2.4f, 1.0f);
                break;
            case Icon::Gear:
            {
                juce::Path p;
                p.setUsingNonZeroWinding (false);
                p.addStar (c, 8, s * 1.15f, s * 1.6f, 0.0f);
                p.addEllipse (c.x - s * 0.55f, c.y - s * 0.55f, s * 1.1f, s * 1.1f);
                g.fillPath (p);
                g.fillEllipse (c.x - s * 1.15f, c.y - s * 1.15f, s * 2.3f, s * 2.3f);
                g.setColour (down ? juce::Colour (0xff4a5059) : over ? juce::Colour (0xff3a4048) : juce::Colour (0xff2c3138));
                g.fillEllipse (c.x - s * 0.55f, c.y - s * 0.55f, s * 1.1f, s * 1.1f);
                break;
            }
        }
    }

private:
    Icon icon;
    bool active = false;
};

// Vertical stereo level meter.
class LevelMeter : public juce::Component
{
public:
    void setLevels (float l, float r)
    {
        left = juce::jmax (l, left * 0.88f);
        right = juce::jmax (r, right * 0.88f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff101316));
        g.fillRoundedRectangle (area, 3.0f);

        auto inner = area.reduced (3.0f);
        const float gap = 2.0f;
        const float w = (inner.getWidth() - gap) / 2.0f;

        auto drawBar = [&g, &inner] (float x, float width, float level)
        {
            const float db = juce::Decibels::gainToDecibels (level, -60.0f);
            const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            const float h = inner.getHeight() * norm;
            g.setColour (db > -1.0f ? juce::Colour (0xffe5484d) : db > -12.0f ? juce::Colour (0xffe2b93b)
                                                                                : juce::Colour (0xff2fb5a2));
            g.fillRect (x, inner.getBottom() - h, width, h);
        };

        drawBar (inner.getX(), w, left);
        drawBar (inner.getX() + w + gap, w, right);
    }

private:
    float left = 0.0f, right = 0.0f;
};

// Numeric peak-hold readout (like Pro Tools). Click to reset.
class PeakReadout : public juce::Component
{
public:
    void update (float peakLinear)
    {
        if (peakLinear > peak)
            peak = peakLinear;
        repaint();
    }

    void reset() { peak = 0.0f; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { reset(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff101316));
        g.fillRoundedRectangle (r, 3.0f);

        const float db = juce::Decibels::gainToDecibels (peak, -100.0f);
        juce::String text = peak <= 0.00001f ? juce::String ("-inf") : juce::String (db, 1);
        g.setColour (db > -0.1f && peak > 0.0f ? juce::Colour (0xffff4d55) : db > -12.0f ? juce::Colour (0xffe2b93b)
                                                                                        : juce::Colour (0xffd4d8de));
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (text, getLocalBounds(), juce::Justification::centred);
    }

private:
    float peak = 0.0f;
};

// One timeline row: channel controls on the left, the audio lane on the right.
class TrackComponent : public juce::Component
{
public:
    struct Callbacks
    {
        std::function<void (std::shared_ptr<Track>)> remove;
        std::function<void (std::shared_ptr<Track>, juce::Component*)> inputMenu;
        std::function<void (std::shared_ptr<Track>, int, juce::Component*)> slotClick;   // name of a slot
        std::function<void (std::shared_ptr<Track>, int, juce::Component*)> slotMenu;    // arrow of a slot
        std::function<void (std::shared_ptr<Track>, juce::Component*)> outputMenu;
    };

    static constexpr int headerWidth = 440;
    static constexpr int rowHeight = 132;

    TrackComponent (Engine& engineToUse, std::shared_ptr<Track> trackToUse, TimelineView& view,
                    Callbacks callbacks, LaneComponent::Callbacks laneCallbacks);

    void paint (juce::Graphics&) override;
    void resized() override;

    void refreshAll();   // button captions from the track state
    void updateLive();   // meters (timer)
    void repaintLane() { lane.repaint(); }

    std::shared_ptr<Track> getTrack() const { return track; }

private:
    Engine& engine;
    std::shared_ptr<Track> track;
    Callbacks cb;

    juce::Label nameLabel;
    IconButton removeButton { IconButton::Icon::Close };
    juce::TextButton inputButton, outputButton;
    StateButton metroButton { "Метроном"_ru, StateButton::Style::Text };

    juce::TextButton slotButtons[Track::numSlots];
    IconButton slotArrows[Track::numSlots] { IconButton (IconButton::Icon::Arrow), IconButton (IconButton::Icon::Arrow),
                                             IconButton (IconButton::Icon::Arrow), IconButton (IconButton::Icon::Arrow),
                                             IconButton (IconButton::Icon::Arrow) };
    bool slotUsed[Track::numSlots] {};

    StateButton muteButton { "MUTE", StateButton::Style::Text };
    StateButton recButton { "REC", StateButton::Style::Dot };
    juce::ToggleButton compButton { "Комп."_ru };

    PeakReadout readout;
    juce::Slider fader;
    LevelMeter meter;

    LaneComponent lane;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackComponent)
};
