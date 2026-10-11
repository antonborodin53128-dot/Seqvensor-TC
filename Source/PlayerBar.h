#pragma once

#include "TrackComponent.h"

// Three plates: sequence (project) | playlist | now playing, with big previous / next buttons on both sides
// of the "now playing" plate. Play / pause / stop live in the top bar.
class PlayerBar : public juce::Component
{
public:
    struct Callbacks
    {
        std::function<void()> sequenceMenu, sequenceSave, sequenceNew;
        std::function<void()> playlistMenu, playlistSave, playlistDelete, playlistAdd;
        std::function<void()> plateClick, previous, next;
    };

    static constexpr int barHeight = 96;

    PlayerBar();

    Callbacks cb;

    void setInfo (const juce::String& sequenceName, const juce::String& playlistName,
                  const juce::String& current, const juce::String& next, const juce::String& counter);

    void setCanStep (bool canPrev, bool canNext);

    juce::Rectangle<int> getSequenceRect() const { return seqRect; }
    juce::Rectangle<int> getPlaylistRect() const { return plRect; }
    juce::Rectangle<int> getPlateRect() const { return plateRect; }
    juce::Rectangle<int> getAddButtonRect() { return plAdd.getBounds(); }
    juce::Rectangle<int> getDeleteButtonRect() { return plDelete.getBounds(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { setHot (0); }
    void mouseDown (const juce::MouseEvent&) override;

private:
    void setHot (int h) { if (h != hot) { hot = h; repaint(); } }
    int hitTest (juce::Point<int> p) const;
    void drawCard (juce::Graphics&, juce::Rectangle<int>, const juce::String& caption, const juce::String& title, bool hovered);

    juce::Rectangle<int> seqRect, plRect, plateRect;
    juce::String seqName, plName, currentTitle, nextTitle, counterText;
    int hot = 0;   // 1 sequence, 2 playlist, 3 plate

    IconButton seqSave { IconButton::Icon::Save }, seqNew { IconButton::Icon::Plus };
    IconButton plSave { IconButton::Icon::Save }, plDelete { IconButton::Icon::Trash }, plAdd { IconButton::Icon::Plus };
    IconButton prevButton { IconButton::Icon::Prev }, nextButton { IconButton::Icon::Next };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayerBar)
};
