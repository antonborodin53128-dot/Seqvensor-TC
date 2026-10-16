#pragma once

#include "TrackComponent.h"

// Left: three tabs (Seq / Playlist / Tracks) with a list under them. Right: the "now playing" plate with the
// big previous / next buttons on both sides. Play / pause / stop live in the top bar.
class PlayerBar : public juce::Component, public juce::DragAndDropTarget, private juce::ListBoxModel
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
        std::function<void (Tab)> save;
        std::function<void()> previous, next, heightChanged;
        std::function<void()> qr;                               // the QR tab: show the code for the phones
        std::function<void()> lyrics;                           // the lyrics button: open the lyrics window
        std::function<void (Tab, int from, int to)> rowMoved;   // a row was dragged to a new place
    };

    static constexpr int closedHeight = 100;
    static constexpr int openHeight = 176;

    PlayerBar();

    Callbacks cb;

    int getPreferredHeight() const { return open ? openHeight : closedHeight; }
    Tab getTab() const { return tab; }
    void showTab (Tab t, bool openList);

    void setRows (Tab t, std::vector<Row> rows);
    std::vector<int> getSelectedIds() const;   // ids of the selected rows of the current tab
    void setNow (const juce::String& current, const juce::String& next, const juce::String& counter);
    void setCanStep (bool canPrev, bool canNext);

    juce::Rectangle<int> getAddButtonScreenBounds() { return localAreaToGlobal (plusButton.getBounds()); }
    juce::Rectangle<int> getDeleteButtonScreenBounds() { return localAreaToGlobal (trashButton.getBounds()); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void paintOverChildren (juce::Graphics&) override;

    // DragAndDropTarget (rows of the list are dragged to a new place)
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    // ListBoxModel
    int getNumRows() override { return (int) rows[(int) tab].size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    // clicks are fired on mouse-up so that a press-and-drag can move a row instead of selecting it
    struct RowWatcher : public juce::MouseListener
    {
        explicit RowWatcher (PlayerBar& o) : owner (o) {}
        void mouseDown (const juce::MouseEvent& e) override { owner.rowDown (e); }
        void mouseDrag (const juce::MouseEvent& e) override { owner.rowDrag (e); }
        void mouseUp (const juce::MouseEvent& e) override { owner.rowUp (e); }
        PlayerBar& owner;
    };
    void rowDown (const juce::MouseEvent&);
    void rowDrag (const juce::MouseEvent&);
    void rowUp (const juce::MouseEvent&);
    int rowAt (const juce::MouseEvent&);
    int rowForDragPosition (juce::Point<int> p) const;
    RowWatcher watcher { *this };
    int pendingRow = -1, dragFrom = -1, dragTo = -1;

    void updateTabUi();
    juce::Rectangle<int> tabRect (int index) const;

    std::vector<Row> rows[3];
    Tab tab = Tab::Tracks;
    bool open = true;

    juce::String currentTitle, nextTitle, counterText;
    juce::Rectangle<int> leftBlock, plateRect;

    juce::TextButton lyricsButton;
    juce::ListBox list;
    IconButton saveButton { IconButton::Icon::Save }, trashButton { IconButton::Icon::Trash }, plusButton { IconButton::Icon::Plus };
    IconButton prevButton { IconButton::Icon::Prev }, nextButton { IconButton::Icon::Next };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayerBar)
};
