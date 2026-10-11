#pragma once

#include "TrackComponent.h"

// Left: three tabs (Seq / Playlist / Tracks) with a list under them. Right: the "now playing" plate with the
// big previous / next buttons on both sides. Play / pause / stop live in the top bar.
class PlayerBar : public juce::Component, private juce::ListBoxModel
{
public:
    enum class Tab { Seq = 0, Playlist = 1, Tracks = 2 };

    struct Row
    {
        juce::String text;
        bool header = false;     // section title, not clickable
        bool current = false;    // the active item of that tab
        int id = -1;
    };

    struct Callbacks
    {
        std::function<void (Tab, int id)> rowClick, rowDouble;
        std::function<void (Tab)> add, remove;
        std::function<void()> save, previous, next, heightChanged;
    };

    static constexpr int closedHeight = 100;
    static constexpr int openHeight = 176;

    PlayerBar();

    Callbacks cb;

    int getPreferredHeight() const { return open ? openHeight : closedHeight; }
    Tab getTab() const { return tab; }
    void showTab (Tab t, bool openList);

    void setRows (Tab t, std::vector<Row> rows);
    void setNow (const juce::String& current, const juce::String& next, const juce::String& counter);
    void setCanStep (bool canPrev, bool canNext);

    juce::Rectangle<int> getAddButtonScreenBounds() { return localAreaToGlobal (plusButton.getBounds()); }
    juce::Rectangle<int> getDeleteButtonScreenBounds() { return localAreaToGlobal (trashButton.getBounds()); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    // ListBoxModel
    int getNumRows() override { return (int) rows[(int) tab].size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    void updateTabUi();
    juce::Rectangle<int> tabRect (int index) const;

    std::vector<Row> rows[3];
    Tab tab = Tab::Tracks;
    bool open = true;

    juce::String currentTitle, nextTitle, counterText;
    juce::Rectangle<int> leftBlock, plateRect;

    juce::ListBox list;
    IconButton saveButton { IconButton::Icon::Save }, trashButton { IconButton::Icon::Trash }, plusButton { IconButton::Icon::Plus };
    IconButton prevButton { IconButton::Icon::Prev }, nextButton { IconButton::Icon::Next };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayerBar)
};
