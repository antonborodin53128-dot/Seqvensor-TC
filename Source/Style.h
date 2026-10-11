#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// One palette and one look for the whole app: dark, soft, rounded squares (Apple-like).
namespace Pal
{
    inline const juce::Colour window     { 0xff000000 };
    inline const juce::Colour surface    { 0xff1f1f1f };
    inline const juce::Colour surfaceHi  { 0xff2e2e2e };
    inline const juce::Colour surfaceLo  { 0xff111111 };
    inline const juce::Colour sunken     { 0xff050505 };
    inline const juce::Colour hairline   { 0x26ffffff };
    inline const juce::Colour text       { 0xfff2f2f7 };
    inline const juce::Colour textDim    { 0xff98989f };
    inline const juce::Colour accent     { 0xffffa81f };
    inline const juce::Colour green      { 0xff30d158 };
    inline const juce::Colour red        { 0xffff453a };
    inline const juce::Colour orange     { 0xffff9f0a };
    inline const juce::Colour violet     { 0xffbf5af2 };
    inline const juce::Colour onAccent   { 0xff000000 };   // text on top of the amber accent
    inline const juce::Colour yellow     { 0xffffd60a };
    inline const juce::Colour cyan       { 0xff6bc7e8 };

    constexpr float radius = 8.0f;        // buttons, fields
    constexpr float radiusBig = 12.0f;    // cards, menus
}

class TcLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TcLookAndFeel()
    {
        setColourScheme (getMidnightColourScheme());
        setDefaultSansSerifTypefaceName ("Segoe UI");

        setColour (juce::ResizableWindow::backgroundColourId, Pal::window);
        setColour (juce::DocumentWindow::textColourId, Pal::text);
        setColour (juce::AlertWindow::backgroundColourId, Pal::surface);
        setColour (juce::AlertWindow::textColourId, Pal::text);
        setColour (juce::AlertWindow::outlineColourId, Pal::hairline);

        setColour (juce::TextButton::buttonColourId, Pal::surface);
        setColour (juce::TextButton::buttonOnColourId, Pal::accent);
        setColour (juce::TextButton::textColourOffId, Pal::text);
        setColour (juce::TextButton::textColourOnId, Pal::onAccent);

        setColour (juce::Label::textColourId, Pal::text);
        setColour (juce::Slider::textBoxTextColourId, Pal::text);
        setColour (juce::Slider::textBoxBackgroundColourId, Pal::sunken);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::thumbColourId, juce::Colours::white);
        setColour (juce::Slider::trackColourId, Pal::accent);
        setColour (juce::Slider::backgroundColourId, Pal::sunken);
        setColour (juce::TextEditor::backgroundColourId, Pal::sunken);
        setColour (juce::TextEditor::outlineColourId, Pal::hairline);
        setColour (juce::TextEditor::focusedOutlineColourId, Pal::accent);
        setColour (juce::ComboBox::backgroundColourId, Pal::surface);
        setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ListBox::backgroundColourId, Pal::surfaceLo);
        setColour (juce::ScrollBar::thumbColourId, juce::Colour (0x60ffffff));
        setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xf0202020));
        setColour (juce::TooltipWindow::textColourId, Pal::text);
        setColour (juce::TooltipWindow::outlineColourId, Pal::hairline);

        // popup menus: semi-transparent colour => the window is not opaque => rounded corners + shadow
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xf2161616));
        setColour (juce::PopupMenu::textColourId, Pal::text);
        setColour (juce::PopupMenu::headerTextColourId, Pal::textDim);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Pal::accent);
        setColour (juce::PopupMenu::highlightedTextColourId, Pal::onAccent);
    }

    // ---- buttons
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::FontOptions (juce::jmin (15.0f, (float) buttonHeight * 0.6f), juce::Font::plain);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& base,
                               bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = base;
        if (! b.isEnabled())
            c = c.withMultipliedAlpha (0.5f);
        else if (down)
            c = c.brighter (0.25f);
        else if (over)
            c = c.brighter (0.12f);

        g.setColour (c);
        g.fillRoundedRectangle (r, Pal::radius);
        g.setColour (Pal::hairline);
        g.drawRoundedRectangle (r, Pal::radius, 1.0f);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool) override
    {
        auto r = b.getLocalBounds().toFloat();
        const float s = juce::jmin (18.0f, r.getHeight() - 4.0f);
        auto box = juce::Rectangle<float> (s, s).withCentre ({ r.getX() + s * 0.5f + 2.0f, r.getCentreY() });

        const bool on = b.getToggleState();
        g.setColour (on ? Pal::accent : (over ? Pal::surfaceHi : Pal::surface));
        g.fillRoundedRectangle (box, 5.0f);
        if (! on)
        {
            g.setColour (Pal::hairline);
            g.drawRoundedRectangle (box, 5.0f, 1.0f);
        }
        else
        {
            juce::Path p;
            p.startNewSubPath (box.getX() + s * 0.26f, box.getCentreY());
            p.lineTo (box.getX() + s * 0.44f, box.getBottom() - s * 0.28f);
            p.lineTo (box.getRight() - s * 0.24f, box.getY() + s * 0.28f);
            g.setColour (Pal::onAccent);
            g.strokePath (p, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour (b.isEnabled() ? Pal::text : Pal::textDim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (b.getButtonText(), r.withTrimmedLeft (s + 8.0f).toNearestInt(), juce::Justification::centredLeft);
    }

    // ---- sliders
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                           float minPos, float maxPos, juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearHorizontal)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, pos, minPos, maxPos, style, s);
            return;
        }

        const bool vertical = style == juce::Slider::LinearVertical;
        const float thick = 4.0f;
        auto area = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);

        juce::Rectangle<float> track = vertical ? juce::Rectangle<float> (thick, area.getHeight()).withCentre (area.getCentre())
                                                : juce::Rectangle<float> (area.getWidth(), thick).withCentre (area.getCentre());
        g.setColour (Pal::sunken);
        g.fillRoundedRectangle (track, thick * 0.5f);

        auto filled = vertical ? track.withTop (pos) : track.withRight (pos);
        g.setColour (Pal::accent);
        g.fillRoundedRectangle (filled, thick * 0.5f);

        auto thumb = vertical ? juce::Rectangle<float> (24.0f, 12.0f).withCentre ({ track.getCentreX(), pos })
                              : juce::Rectangle<float> (12.0f, 24.0f).withCentre ({ pos, track.getCentreY() });
        g.setColour (juce::Colour (0x55000000));
        g.fillRoundedRectangle (thumb.translated (0.0f, 1.0f), 4.0f);
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (thumb, 4.0f);
    }

    // ---- scrollbars: thin rounded pill
    void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                        int thumbStart, int thumbSize, bool over, bool down) override
    {
        auto thumb = vertical ? juce::Rectangle<float> ((float) x, (float) (y + thumbStart), (float) w, (float) thumbSize)
                              : juce::Rectangle<float> ((float) (x + thumbStart), (float) y, (float) thumbSize, (float) h);
        thumb = thumb.reduced (vertical ? 2.5f : 0.0f, vertical ? 0.0f : 2.5f);
        g.setColour (juce::Colours::white.withAlpha (down ? 0.55f : over ? 0.42f : 0.28f));
        g.fillRoundedRectangle (thumb, juce::jmin (thumb.getWidth(), thumb.getHeight()) * 0.5f);
    }

    // ---- combo box
    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, Pal::radius);
        g.setColour (Pal::hairline);
        g.drawRoundedRectangle (r, Pal::radius, 1.0f);

        juce::Path p;
        const float cx = (float) w - 16.0f, cy = (float) h * 0.5f;
        p.startNewSubPath (cx - 4.0f, cy - 2.0f);
        p.lineTo (cx, cy + 2.5f);
        p.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (Pal::textDim);
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // ---- tooltip
    void drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h) override
    {
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
        g.setColour (juce::Colour (0xf0202020));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (Pal::hairline);
        g.drawRoundedRectangle (r, 8.0f, 1.0f);

        juce::AttributedString s;
        s.setJustification (juce::Justification::centredLeft);
        s.append (text, juce::FontOptions (13.0f), Pal::text);
        juce::TextLayout tl;
        tl.createLayout (s, (float) w - 20.0f);
        tl.draw (g, r.reduced (10.0f, 6.0f));
    }

    // ---- popup menu: floating rounded panel, rounded highlight
    int getPopupMenuBorderSize() override { return 8; }

    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
        g.setColour (findColour (juce::PopupMenu::backgroundColourId));
        g.fillRoundedRectangle (r, Pal::radiusBig);
        g.setColour (juce::Colour (0x26ffffff));
        g.drawRoundedRectangle (r, Pal::radiusBig, 1.0f);
    }

    juce::Font getPopupMenuFont() override { return juce::FontOptions (15.0f); }

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int, int& idealWidth, int& idealHeight) override
    {
        if (isSeparator)
        {
            idealWidth = 50;
            idealHeight = 13;
            return;
        }
        juce::GlyphArrangement ga;
        ga.addLineOfText (getPopupMenuFont(), text, 0.0f, 0.0f);
        idealWidth = (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth()) + 64;
        idealHeight = 32;
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcut, const juce::Drawable*, const juce::Colour* textColour) override
    {
        if (isSeparator)
        {
            g.setColour (Pal::hairline.withAlpha (0.16f));
            g.fillRect (area.getX() + 10, area.getCentreY(), area.getWidth() - 20, 1);
            return;
        }

        auto r = area.reduced (0, 1).toFloat();
        const bool hot = isHighlighted && isActive;
        if (hot)
        {
            g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
            g.fillRoundedRectangle (r, 7.0f);
        }

        juce::Colour col = textColour != nullptr ? *textColour : findColour (juce::PopupMenu::textColourId);
        if (hot)
            col = Pal::onAccent;
        if (! isActive)
            col = col.withMultipliedAlpha (0.4f);
        g.setColour (col);

        if (isTicked)
        {
            juce::Path p;
            const float cx = r.getX() + 16.0f, cy = r.getCentreY();
            p.startNewSubPath (cx - 5.0f, cy);
            p.lineTo (cx - 1.5f, cy + 4.0f);
            p.lineTo (cx + 5.0f, cy - 4.5f);
            g.strokePath (p, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setFont (getPopupMenuFont());
        g.drawText (text, r.withTrimmedLeft (32.0f).withTrimmedRight (hasSubMenu ? 26.0f : 12.0f).toNearestInt(),
                    juce::Justification::centredLeft, true);

        if (shortcut.isNotEmpty())
        {
            g.setColour (col.withMultipliedAlpha (0.6f));
            g.setFont (juce::FontOptions (13.0f));
            g.drawText (shortcut, r.withTrimmedRight (12.0f).toNearestInt(), juce::Justification::centredRight);
        }

        if (hasSubMenu)
        {
            juce::Path p;
            const float cx = r.getRight() - 14.0f, cy = r.getCentreY();
            p.startNewSubPath (cx - 2.5f, cy - 4.5f);
            p.lineTo (cx + 2.5f, cy);
            p.lineTo (cx - 2.5f, cy + 4.5f);
            g.setColour (col.withMultipliedAlpha (0.8f));
            g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }
};
