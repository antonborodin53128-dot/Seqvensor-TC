#include "PlayerBar.h"
#include "Text.h"

PlayerBar::PlayerBar()
{
    seqSave.setTooltip ("Сохранить секвенцию"_ru);
    seqSave.onClick = [this] { if (cb.sequenceSave) cb.sequenceSave(); };
    seqNew.setTooltip ("Новая секвенция"_ru);
    seqNew.onClick = [this] { if (cb.sequenceNew) cb.sequenceNew(); };
    plSave.setTooltip ("Сохранить плейлист"_ru);
    plSave.onClick = [this] { if (cb.playlistSave) cb.playlistSave(); };
    plDelete.setTooltip ("Удалить: песню из плейлиста, плейлист или секвенцию"_ru);
    plDelete.onClick = [this] { if (cb.playlistDelete) cb.playlistDelete(); };
    plAdd.setTooltip ("Добавить трек в плейлист"_ru);
    plAdd.onClick = [this] { if (cb.playlistAdd) cb.playlistAdd(); };
    prevButton.setTooltip ("Предыдущая песня"_ru);
    prevButton.onClick = [this] { if (cb.previous) cb.previous(); };
    nextButton.setTooltip ("Следующая песня"_ru);
    nextButton.onClick = [this] { if (cb.next) cb.next(); };

    for (auto* b : { &seqSave, &seqNew, &plSave, &plDelete, &plAdd, &prevButton, &nextButton })
        addAndMakeVisible (b);
}

void PlayerBar::setInfo (const juce::String& s, const juce::String& p, const juce::String& cur,
                         const juce::String& nxt, const juce::String& counter)
{
    seqName = s; plName = p; currentTitle = cur; nextTitle = nxt; counterText = counter;
    repaint();
}

void PlayerBar::setCanStep (bool canPrev, bool canNext)
{
    prevButton.setEnabled (canPrev);
    nextButton.setEnabled (canNext);
    prevButton.setAlpha (canPrev ? 1.0f : 0.35f);
    nextButton.setAlpha (canNext ? 1.0f : 0.35f);
}

void PlayerBar::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    const int gap = 10, bigW = 84;

    seqRect = r.removeFromLeft (240);
    r.removeFromLeft (gap);
    plRect = r.removeFromLeft (252);
    r.removeFromLeft (gap);

    prevButton.setBounds (r.removeFromLeft (bigW));
    r.removeFromLeft (gap);
    nextButton.setBounds (r.removeFromRight (bigW));
    r.removeFromRight (gap);
    plateRect = r;

    // small icon buttons in the top-right corner of a plate, right to left
    auto place = [] (juce::Rectangle<int> card, std::initializer_list<IconButton*> buttons)
    {
        int x = card.getRight() - 8;
        for (auto* b : buttons)
        {
            b->setBounds (x - 28, card.getY() + 7, 28, 22);
            x -= 28 + 4;
        }
    };
    place (seqRect, { &seqNew, &seqSave });
    place (plRect, { &plAdd, &plDelete, &plSave });
}

void PlayerBar::drawCard (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& caption,
                          const juce::String& title, bool hovered)
{
    auto rf = r.toFloat();
    g.setColour (hovered ? Pal::surface : Pal::surfaceLo);
    g.fillRoundedRectangle (rf, Pal::radiusBig);
    g.setColour (Pal::hairline);
    g.drawRoundedRectangle (rf.reduced (0.5f), Pal::radiusBig, 1.0f);

    g.setColour (Pal::textDim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (caption, r.getX() + 14, r.getY() + 9, r.getWidth() - 28, 14, juce::Justification::left);

    g.setColour (Pal::text);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText (title, r.getX() + 14, r.getY() + 34, r.getWidth() - 28, 28, juce::Justification::left, true);
}

void PlayerBar::paint (juce::Graphics& g)
{
    drawCard (g, seqRect, "СЕКВЕНЦИЯ"_ru, seqName, hot == 1);
    drawCard (g, plRect, "ПЛЕЙЛИСТ"_ru, plName, hot == 2);

    auto r = plateRect;
    g.setColour (hot == 3 ? Pal::surface : Pal::surfaceLo);
    g.fillRoundedRectangle (r.toFloat(), Pal::radiusBig);
    g.setColour (Pal::hairline);
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), Pal::radiusBig, 1.0f);

    g.setColour (Pal::textDim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("СЕЙЧАС ИГРАЕТ"_ru, r.getX() + 18, r.getY() + 9, 200, 14, juce::Justification::left);
    g.drawText (counterText, r.getRight() - 118, r.getY() + 9, 100, 14, juce::Justification::right);

    // the current song, big; the next one below it: smaller and dimmer (like music players)
    g.setColour (Pal::text);
    g.setFont (juce::FontOptions (25.0f, juce::Font::bold));
    g.drawText (currentTitle, r.getX() + 18, r.getY() + 25, r.getWidth() - 36, 32, juce::Justification::left, true);

    g.setColour (Pal::textDim.withAlpha (0.65f));
    g.setFont (juce::FontOptions (14.0f));
    g.drawText ("Далее: "_ru + nextTitle, r.getX() + 18, r.getY() + 59, r.getWidth() - 36, 20, juce::Justification::left, true);
}

int PlayerBar::hitTest (juce::Point<int> p) const
{
    if (seqRect.contains (p)) return 1;
    if (plRect.contains (p)) return 2;
    if (plateRect.contains (p)) return 3;
    return 0;
}

void PlayerBar::mouseMove (const juce::MouseEvent& e)
{
    // the cards react only above their icon rows
    auto p = e.getPosition();
    int h = hitTest (p);
    if ((h == 1 && p.y < seqRect.getY() + 32 && p.x > seqRect.getRight() - 140) || (h == 2 && p.y < plRect.getY() + 32 && p.x > plRect.getRight() - 140))
        h = 0;
    setHot (h);
}

void PlayerBar::mouseDown (const juce::MouseEvent& e)
{
    auto p = e.getPosition();
    const int h = hitTest (p);
    if (h == 1 && cb.sequenceMenu)
        cb.sequenceMenu();
    else if (h == 2 && cb.playlistMenu)
        cb.playlistMenu();
    else if (h == 3 && cb.plateClick)
        cb.plateClick();
}
