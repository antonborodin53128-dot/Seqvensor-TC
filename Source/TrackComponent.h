#pragma once

#include "Engine.h"
#include "Timeline.h"
#include "Style.h"

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

        g.setColour (on ? onColour : (over ? offColour.brighter (0.15f) : offColour));
        g.fillRoundedRectangle (r, Pal::radius);
        g.setColour (on ? onEdgeColour.withAlpha (0.5f) : offEdgeColour);
        g.drawRoundedRectangle (r, Pal::radius, 1.0f);

        if (style == Style::Text)
        {
            g.setColour (on ? juce::Colours::white : offTextColour);
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred);
        }
        else
        {
            const float d = juce::jmin (r.getHeight() - 10.0f, 18.0f);
            auto dot = juce::Rectangle<float> (d, d).withCentre (r.getCentre());
            g.setColour (on ? juce::Colours::white : Pal::red.withAlpha (0.6f));
            g.fillEllipse (dot);
        }
    }

private:
    Style style;
    juce::Colour onColour { Pal::red }, offColour { 0xff3e2a2c }, onEdgeColour { 0xffffb4ae },
                 offEdgeColour { 0x1fffffff }, offTextColour { 0xffd6908a };
};

// Small drawn icon button (no font glyphs, so nothing can turn into "hieroglyphs").
class IconButton : public juce::Button
{
public:
    enum class Icon { Play, Pause, Stop, Close, Gear, Menu, Arrow, Prev, Next, Save, Trash, Plus, Record, Power };

    explicit IconButton (Icon i) : juce::Button ({}), icon (i) {}

    void setIcon (Icon i) { icon = i; repaint(); }
    void setActive (bool a) { if (a != active) { active = a; repaint(); } }
    void setSpin (float angle) { spin = angle; repaint(); }   // Record icon: rotating ring while recording

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (active ? (icon == Icon::Record ? Pal::red.darker (0.2f) : Pal::green.darker (0.25f))
                            : down ? Pal::surfaceHi.brighter (0.2f) : over ? Pal::surfaceHi : Pal::surface);
        g.fillRoundedRectangle (r, Pal::radius);
        g.setColour (Pal::hairline);
        g.drawRoundedRectangle (r, Pal::radius, 1.0f);

        g.setColour (Pal::text);
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
            case Icon::Prev:
            case Icon::Next:
            {
                // d = direction the triangle points to; the bar is on the same side
                const float d = icon == Icon::Prev ? -1.0f : 1.0f;
                juce::Path p;
                p.addTriangle (c.x + d * s * 0.8f, c.y, c.x - d * s * 0.9f, c.y - s * 1.1f, c.x - d * s * 0.9f, c.y + s * 1.1f);
                g.fillPath (p);
                g.fillRoundedRectangle (juce::Rectangle<float> (s * 0.4f, s * 2.2f).withCentre ({ c.x + d * s * 1.15f, c.y }), 1.5f);
                break;
            }
            case Icon::Save:
            {
                auto body = juce::Rectangle<float> (s * 2.2f, s * 2.2f).withCentre (c);
                g.drawRoundedRectangle (body, 3.0f, 1.8f);
                g.fillRect (juce::Rectangle<float> (s * 1.1f, s * 0.7f).withCentre ({ c.x, body.getY() + s * 0.65f }));
                g.fillRoundedRectangle (juce::Rectangle<float> (s * 1.3f, s * 0.8f).withCentre ({ c.x, body.getBottom() - s * 0.7f }), 1.5f);
                break;
            }
            case Icon::Trash:
            {
                g.fillRoundedRectangle (juce::Rectangle<float> (s * 2.4f, s * 0.35f).withCentre ({ c.x, c.y - s * 0.95f }), 1.0f);
                g.fillRoundedRectangle (juce::Rectangle<float> (s * 0.9f, s * 0.4f).withCentre ({ c.x, c.y - s * 1.3f }), 1.0f);
                g.drawRoundedRectangle (juce::Rectangle<float> (s * 1.8f, s * 1.9f).withCentre ({ c.x, c.y + s * 0.2f }), 2.5f, 1.8f);
                break;
            }
            case Icon::Record:
                g.setColour (active ? juce::Colours::white : Pal::red);
                g.fillEllipse (c.x - s * 1.1f, c.y - s * 1.1f, s * 2.2f, s * 2.2f);
                if (active)
                {
                    juce::Path ring;
                    const float rr = s * 1.9f;
                    ring.addCentredArc (c.x, c.y, rr, rr, 0.0f, spin, spin + 1.9f, true);
                    ring.addCentredArc (c.x, c.y, rr, rr, 0.0f, spin + juce::MathConstants<float>::pi, spin + juce::MathConstants<float>::pi + 1.9f, true);
                    g.strokePath (ring, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
                break;
            case Icon::Power:
            {
                const float rr = s * 1.15f;
                juce::Path arc;
                arc.addCentredArc (c.x, c.y + s * 0.1f, rr, rr, 0.0f, 0.6f, juce::MathConstants<float>::twoPi - 0.6f, true);
                g.strokePath (arc, juce::PathStrokeType (2.0f));
                g.drawLine (c.x, c.y - s * 1.3f, c.x, c.y - s * 0.1f, 2.0f);
                break;
            }
            case Icon::Plus:
                g.fillRoundedRectangle (juce::Rectangle<float> (s * 2.2f, 2.2f).withCentre (c), 1.0f);
                g.fillRoundedRectangle (juce::Rectangle<float> (2.2f, s * 2.2f).withCentre (c), 1.0f);
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
                g.setColour (down ? Pal::surfaceHi.brighter (0.2f) : over ? Pal::surfaceHi : Pal::surface);
                g.fillEllipse (c.x - s * 0.55f, c.y - s * 0.55f, s * 1.1f, s * 1.1f);
                break;
            }
        }
    }

private:
    Icon icon;
    bool active = false;
    float spin = 0.0f;
};

// Grip at the left of the track name: press and drag to move the whole track up / down.
class GripHandle : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onStart;   // the press turned into a drag

    GripHandle() { setMouseCursor (juce::MouseCursor::DraggingHandCursor); }

    void paint (juce::Graphics& g) override
    {
        g.setColour (hot ? Pal::text : Pal::textDim);
        auto c = getLocalBounds().toFloat().getCentre();
        for (int row = -1; row <= 1; ++row)   // 3 rows x 2 columns = 6 dots
            for (int col = -1; col <= 0; ++col)
                g.fillEllipse (c.x + (float) col * 5.0f + 0.5f, c.y + (float) row * 6.0f - 1.5f, 3.0f, 3.0f);
    }
    void mouseEnter (const juce::MouseEvent&) override { hot = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hot = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { started = false; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! started && e.getDistanceFromDragStart() > 5 && onStart)
        {
            started = true;
            onStart();
        }
    }

private:
    bool hot = false, started = false;
};

// Padlock: locked = nothing on the timeline and the tracks can be changed (protection against stray clicks).
class LockButton : public juce::Button
{
public:
    LockButton() : juce::Button ("lock") { setClickingTogglesState (true); }

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        const bool locked = getToggleState();
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (locked ? Pal::orange.withAlpha (0.9f) : (over ? Pal::surfaceHi : Pal::surface));
        g.fillRoundedRectangle (r, Pal::radius);
        g.setColour (Pal::hairline);
        g.drawRoundedRectangle (r, Pal::radius, 1.0f);

        const auto c = r.getCentre();
        const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.22f;
        g.setColour (locked ? juce::Colours::black : Pal::text);
        // body
        g.fillRoundedRectangle (juce::Rectangle<float> (s * 2.6f, s * 2.0f).withCentre ({ c.x, c.y + s * 0.7f }), 1.5f);
        // shackle: closed when locked, lifted and open when not
        juce::Path p;
        const float top = locked ? c.y - s * 1.5f : c.y - s * 2.1f;
        p.startNewSubPath (c.x - s * 0.85f, c.y - s * 0.3f);
        p.lineTo (c.x - s * 0.85f, top + s * 0.8f);
        p.quadraticTo (c.x - s * 0.85f, top, c.x, top);
        p.quadraticTo (c.x + s * 0.85f, top, c.x + s * 0.85f, top + s * 0.8f);
        p.lineTo (c.x + s * 0.85f, locked ? c.y - s * 0.3f : top + s * 1.1f);
        g.strokePath (p, juce::PathStrokeType (1.8f));
    }
};

// Transparent cover over the controls of a track while the lock is on: it swallows every click.
class LockShield : public juce::Component
{
public:
    void mouseDown (const juce::MouseEvent&) override {}
};

// Vertical stereo level meter.
class LevelMeter : public juce::Component
{
public:
    // peak: instantaneous peak of the last interval; rms: its RMS. The bar shows the loudness (VU-like, smoothed);
    // a thin line shows the peak and holds it for a moment.
    void setLevels (float peakL, float peakR, float rmsL, float rmsR)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = lastTime > 0.0 ? (float) juce::jlimit (1.0, 200.0, now - lastTime) : 33.0f;
        lastTime = now;
        const float attack = 1.0f - std::exp (-dt / 40.0f), release = 1.0f - std::exp (-dt / 300.0f);
        auto follow = [&] (float& shown, float in) { shown += (in - shown) * (in > shown ? attack : release); };
        follow (barL, rmsL * 1.4142f);   // +3 dB: a full-scale sine reads 0 dB
        follow (barR, rmsR * 1.4142f);
        auto hold = [&] (float& h, double& at, float in)
        {
            if (in >= h)
            {
                h = in;
                at = now;
            }
            else if (now - at > 1200.0)   // held for 1.2 s, then it falls
                h = juce::jmax (in, h * std::exp (-dt / 250.0f));
        };
        hold (holdL, holdAtL, peakL);
        hold (holdR, holdAtR, peakR);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (Pal::sunken);
        g.fillRoundedRectangle (area, 6.0f);

        auto inner = area.reduced (3.0f);
        const float gap = 2.0f;
        const float w = (inner.getWidth() - gap) / 2.0f;

        auto norm = [] (float level) { return juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (level, -48.0f) + 48.0f) / 48.0f); };
        auto drawBar = [&] (float x, float width, float level, float peak)
        {
            const float db = juce::Decibels::gainToDecibels (level, -48.0f);
            const float h = inner.getHeight() * norm (level);
            g.setColour (db > -3.0f ? Pal::red : db > -12.0f ? Pal::yellow : Pal::green);
            g.fillRoundedRectangle (x, inner.getBottom() - h, width, h, 1.5f);
            if (peak > 0.0005f)
            {
                const float py = inner.getBottom() - inner.getHeight() * norm (peak);
                g.setColour (juce::Decibels::gainToDecibels (peak, -48.0f) > -1.0f ? Pal::red : Pal::text);
                g.fillRect (x, py, width, 1.5f);
            }
        };

        drawBar (inner.getX(), w, barL, holdL);
        drawBar (inner.getX() + w + gap, w, barR, holdR);
    }

private:
    float barL = 0.0f, barR = 0.0f, holdL = 0.0f, holdR = 0.0f;
    double holdAtL = 0.0, holdAtR = 0.0, lastTime = 0.0;
};

// Numeric readout of the fader position in dB (what the fader is set to, not the level of the audio).
class PeakReadout : public juce::Component
{
public:
    void setFaderDb (double db)
    {
        if (db != faderDb)
        {
            faderDb = db;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Pal::sunken);
        g.fillRoundedRectangle (r, 6.0f);

        const bool off = faderDb <= -59.9;
        juce::String text = off ? juce::String ("-inf") : (faderDb > 0.05 ? "+" : juce::String()) + juce::String (faderDb, 1);
        g.setColour (off ? Pal::textDim : faderDb > 0.05 ? Pal::yellow : Pal::text);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (text, getLocalBounds(), juce::Justification::centred);
    }

private:
    double faderDb = 0.0;
};

// Small pan knob: dark round body, orange pointer, thin ring.
class PanKnobLook : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override
    {
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
        const float d = juce::jmin (r.getWidth(), r.getHeight());
        r = juce::Rectangle<float> (d, d).withCentre (r.getCentre());
        const float ang = a0 + pos * (a1 - a0);

        juce::ColourGradient grad (juce::Colour (0xff34383c), r.getX(), r.getY(), juce::Colour (0xff15181a), r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillEllipse (r);
        g.setColour (juce::Colour (0xff0b0d0e));
        g.drawEllipse (r, 1.5f);

        const auto c = r.getCentre();
        const float rad = d * 0.5f;
        g.setColour (Pal::accent);
        g.drawLine (c.x + std::sin (ang) * rad * 0.40f, c.y - std::cos (ang) * rad * 0.40f,
                    c.x + std::sin (ang) * rad * 0.88f, c.y - std::cos (ang) * rad * 0.88f, 2.5f);
    }
};

// Plugin slot: click opens the plugin, press and drag moves it (Ctrl while dropping = copy).
class SlotButton : public juce::TextButton, public juce::DragAndDropTarget
{
public:
    std::function<juce::var()> dragPayload;                                  // void = this slot cannot be dragged
    std::function<void (const juce::String& payload, bool copy)> onDropped;
    std::function<void()> onRightClick;                                      // right click = bypass

    bool consumeDragFlag() { const bool d = justDragged; justDragged = false; return d; }

    void mouseDown (const juce::MouseEvent& e) override
    {
        justDragged = false;
        rightPressed = e.mods.isPopupMenu();
        if (rightPressed)
        {
            if (onRightClick)
                onRightClick();
            return;
        }
        juce::TextButton::mouseDown (e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (rightPressed)
        {
            rightPressed = false;   // right button = bypass only, never a click
            return;
        }
        juce::TextButton::mouseUp (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (rightPressed)
            return;
        juce::TextButton::mouseDrag (e);
        if (justDragged || e.getDistanceFromDragStart() < 8 || ! dragPayload)
            return;
        const auto payload = dragPayload();
        if (payload.isVoid())
            return;
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            justDragged = true;
            container->startDragging (payload, this, juce::ScaledImage (createComponentSnapshot (getLocalBounds())), true);
        }
    }

    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString().startsWith ("tcslot:"); }
    void itemDragEnter (const SourceDetails&) override { dropHover = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { dropHover = false; repaint(); }
    void itemDropped (const SourceDetails& d) override
    {
        dropHover = false;
        repaint();
        const auto mods = juce::ModifierKeys::currentModifiers;
        if (onDropped)
            onDropped (d.description.toString(), mods.isCtrlDown() || mods.isCommandDown());
    }
    void paintOverChildren (juce::Graphics& g) override
    {
        if (! dropHover)
            return;
        g.setColour (juce::Colour (0xffffa81f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f, 2.0f);
    }

private:
    bool justDragged = false, dropHover = false, rightPressed = false;
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
        std::function<void (std::shared_ptr<Track>, int)> slotBypass;                    // power button of a slot
        std::function<void (std::shared_ptr<Track>, int, const juce::String&, bool)> slotDropped;   // plugin dropped on a slot (payload, copy)
        std::function<void (std::shared_ptr<Track>, juce::Component*)> outputMenu;
    };

    static constexpr int headerWidth = 480;
    static constexpr int rowHeight = 132;

    TrackComponent (Engine& engineToUse, std::shared_ptr<Track> trackToUse, TimelineView& view,
                    Callbacks callbacks, LaneComponent::Callbacks laneCallbacks);

    void paint (juce::Graphics&) override;
    void resized() override;

    void refreshAll();   // button captions from the track state
    void updateLive();   // meters (timer)
    void repaintLane() { lane.repaint(); }
    void endDragVisual() { if (getAlpha() < 1.0f) setAlpha (1.0f); }
    void setLocked (bool l) { shield.setVisible (l); if (l) shield.toFront (false); }
    LaneComponent& getLane() { return lane; }

    std::shared_ptr<Track> getTrack() const { return track; }

private:
    Engine& engine;
    std::shared_ptr<Track> track;
    Callbacks cb;

    GripHandle grip;
    juce::Label nameLabel;
    IconButton removeButton { IconButton::Icon::Close };
    juce::TextButton inputButton, outputButton;
    StateButton metroButton { "Метроном"_ru, StateButton::Style::Text };

    SlotButton slotButtons[Track::numSlots];
    IconButton slotArrows[Track::numSlots] { IconButton (IconButton::Icon::Arrow), IconButton (IconButton::Icon::Arrow),
                                             IconButton (IconButton::Icon::Arrow), IconButton (IconButton::Icon::Arrow),
                                             IconButton (IconButton::Icon::Arrow) };
    bool slotUsed[Track::numSlots] {};

    StateButton muteButton { "MUTE", StateButton::Style::Text };
    StateButton soloButton { "SOLO", StateButton::Style::Text };
    PanKnobLook panLook;
    juce::Slider panSlider;
    StateButton recButton { "REC", StateButton::Style::Dot };
    juce::ToggleButton compButton { "Комп."_ru };
    StateButton monoButton { "ST", StateButton::Style::Text };   // MONO / ST (stereo)

    PeakReadout readout;
    juce::Slider fader;
    LevelMeter meter;

    LaneComponent lane;
    LockShield shield;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackComponent)
};

// The list of track strips: shows where a dragged strip would land and accepts the drop.
class TrackListArea : public juce::Component, public juce::DragAndDropTarget
{
public:
    std::function<void (const juce::String&, juce::Point<int>)> onMove, onDrop;
    std::function<void()> onLeave;

    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString().startsWith ("tctrack:"); }
    void itemDragEnter (const SourceDetails& d) override { if (onMove) onMove (d.description.toString(), d.localPosition); }
    void itemDragMove (const SourceDetails& d) override { if (onMove) onMove (d.description.toString(), d.localPosition); }
    void itemDragExit (const SourceDetails&) override { if (onLeave) onLeave(); }
    void itemDropped (const SourceDetails& d) override { if (onDrop) onDrop (d.description.toString(), d.localPosition); }
};
