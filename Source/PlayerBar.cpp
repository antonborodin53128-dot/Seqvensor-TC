#include "PlayerBar.h"
#include "Text.h"

namespace
{
    const char* tabNames[3] = { "Seq", "Playlist", "Tracks" };
}

PlayerBar::PlayerBar() : list ("songs", nullptr)
{
    list.setModel (this);
    list.setRowHeight (28);
    list.setMultipleSelectionEnabled (true);
    list.setWantsKeyboardFocus (false);   // Space and the arrows belong to the main window
    list.setOutlineThickness (0);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (list);
    list.addMouseListener (&watcher, true);

    saveButton.setTooltip ("Сохранить в файл выбранное: секвенцию, плейлист или трек"_ru);
    saveButton.onClick = [this] { if (cb.save) cb.save (tab); };
    trashButton.onClick = [this] { if (cb.remove) cb.remove (tab); };
    plusButton.onClick = [this] { if (cb.add) cb.add (tab); };
    prevButton.setTooltip ("Предыдущий трек"_ru);
    prevButton.onClick = [this] { if (cb.previous) cb.previous(); };
    nextButton.setTooltip ("Следующий трек"_ru);
    nextButton.onClick = [this] { if (cb.next) cb.next(); };

    for (auto* b : { &saveButton, &trashButton, &plusButton, &prevButton, &nextButton })
        addAndMakeVisible (b);

    updateTabUi();
}

void PlayerBar::showTab (Tab t, bool openList)
{
    const bool heightChanges = openList != open;
    tab = t;
    open = openList;
    list.deselectAllRows();
    updateTabUi();
    resized();
    repaint();
    if (heightChanges && cb.heightChanged)
        cb.heightChanged();
}

void PlayerBar::updateTabUi()
{
    switch (tab)
    {
        case Tab::Seq:
            plusButton.setTooltip ("Новая секвенция"_ru);
            trashButton.setTooltip ("Удалить секвенцию"_ru);
            break;
        case Tab::Playlist:
            plusButton.setTooltip ("Новый плейлист"_ru);
            trashButton.setTooltip ("Удалить плейлист"_ru);
            break;
        case Tab::Tracks:
            plusButton.setTooltip ("Добавить трек в плейлист"_ru);
            trashButton.setTooltip ("Удалить трек: из плейлиста или с диска"_ru);
            break;
    }
    list.setVisible (open);
    list.updateContent();
    list.repaint();
}

void PlayerBar::setRows (Tab t, std::vector<Row> newRows)
{
    rows[(int) t] = std::move (newRows);
    if (t == tab)
    {
        list.updateContent();
        list.repaint();

        // keep the active item in view
        const auto& r = rows[(int) t];
        for (int i = 0; i < (int) r.size(); ++i)
            if (r[(size_t) i].current)
            {
                list.scrollToEnsureRowIsOnscreen (i);
                break;
            }
    }
    repaint();
}

void PlayerBar::setNow (const juce::String& cur, const juce::String& nxt, const juce::String& counter)
{
    currentTitle = cur;
    nextTitle = nxt;
    counterText = counter;
    repaint();
}

void PlayerBar::setCanStep (bool canPrev, bool canNext)
{
    prevButton.setEnabled (canPrev);
    nextButton.setEnabled (canNext);
    prevButton.setAlpha (canPrev ? 1.0f : 0.35f);
    nextButton.setAlpha (canNext ? 1.0f : 0.35f);
}

juce::Rectangle<int> PlayerBar::tabRect (int index) const
{
    return juce::Rectangle<int> (leftBlock.getX() + index * 94, leftBlock.getY(), 88, 30);
}

void PlayerBar::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    const int gap = 10, bigW = 84;

    leftBlock = r.removeFromLeft (juce::jmin (520, r.getWidth() / 2));
    r.removeFromLeft (gap);

    prevButton.setBounds (r.removeFromLeft (bigW));
    r.removeFromLeft (gap);
    nextButton.setBounds (r.removeFromRight (bigW));
    r.removeFromRight (gap);
    plateRect = r;

    // tab row: [Seq] [Playlist] [Tracks] ............ [save] [delete] [add]
    auto tabsRow = leftBlock.withHeight (30);
    plusButton.setBounds (tabsRow.removeFromRight (34).reduced (0, 3));
    tabsRow.removeFromRight (4);
    trashButton.setBounds (tabsRow.removeFromRight (34).reduced (0, 3));
    tabsRow.removeFromRight (4);
    saveButton.setBounds (tabsRow.removeFromRight (34).reduced (0, 3));

    list.setBounds (leftBlock.withTrimmedTop (36));
}

void PlayerBar::paint (juce::Graphics& g)
{
    // tabs
    for (int i = 0; i < 3; ++i)
    {
        auto tr = tabRect (i).toFloat();
        const bool on = i == (int) tab;
        g.setColour (on ? Pal::accent : Pal::surface);
        g.fillRoundedRectangle (tr, Pal::radius);
        if (! on)
        {
            g.setColour (Pal::hairline);
            g.drawRoundedRectangle (tr.reduced (0.5f), Pal::radius, 1.0f);
        }
        g.setColour (on ? Pal::onAccent : Pal::text);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (tabNames[i], tr.toNearestInt(), juce::Justification::centred);
    }

    // list background / collapsed summary
    auto body = leftBlock.withTrimmedTop (36);
    g.setColour (Pal::surfaceLo);
    g.fillRoundedRectangle (body.toFloat(), Pal::radiusBig);
    g.setColour (Pal::hairline);
    g.drawRoundedRectangle (body.toFloat().reduced (0.5f), Pal::radiusBig, 1.0f);

    if (! open)
    {
        juce::String current;
        for (auto& row : rows[(int) tab])
            if (row.current)
                current = row.text;
        g.setColour (Pal::text);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawText (current.isEmpty() ? juce::String ("-") : current, body.reduced (14, 0), juce::Justification::centredLeft, true);
    }

    // now playing: the current track big, the next one below it smaller and dimmer
    auto r = plateRect;
    g.setColour (Pal::surfaceLo);
    g.fillRoundedRectangle (r.toFloat(), Pal::radiusBig);
    g.setColour (Pal::hairline);
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), Pal::radiusBig, 1.0f);

    auto inner = r.reduced (18, 10);
    g.setColour (Pal::textDim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("СЕЙЧАС ИГРАЕТ"_ru, inner.removeFromTop (14), juce::Justification::left);
    g.drawText (counterText, r.getRight() - 118, r.getY() + 10, 100, 14, juce::Justification::right);

    const int blockH = 32 + 4 + 20;
    auto mid = inner.withSizeKeepingCentre (inner.getWidth(), juce::jmin (blockH, inner.getHeight()));
    g.setColour (Pal::text);
    g.setFont (juce::FontOptions (25.0f, juce::Font::bold));
    g.drawText (currentTitle, mid.removeFromTop (32), juce::Justification::left, true);
    mid.removeFromTop (4);
    g.setColour (Pal::textDim.withAlpha (0.65f));
    g.setFont (juce::FontOptions (14.0f));
    g.drawText ("Далее: "_ru + nextTitle, mid.removeFromTop (20), juce::Justification::left, true);
}

void PlayerBar::mouseDown (const juce::MouseEvent& e)
{
    if (e.eventComponent != this)
        return;
    for (int i = 0; i < 3; ++i)
        if (tabRect (i).contains (e.getPosition()))
        {
            // the selected tab again = fold / unfold the list
            showTab ((Tab) i, i == (int) tab ? ! open : true);
            return;
        }
}

void PlayerBar::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    const auto& list_ = rows[(int) tab];
    if (row < 0 || row >= (int) list_.size())
        return;
    const auto& item = list_[(size_t) row];
    if (row == dragFrom && dragTo >= 0)
        g.setOpacity (0.35f);

    auto r = juce::Rectangle<float> (4.0f, 1.0f, (float) w - 8.0f, (float) h - 2.0f);
    if (item.header)
    {
        g.setColour (Pal::textDim);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (item.text.toUpperCase(), r.reduced (10.0f, 0.0f).toNearestInt(), juce::Justification::bottomLeft);
        return;
    }

    if (item.current)
    {
        g.setColour (Pal::accent.withAlpha (0.2f));
        g.fillRoundedRectangle (r, 7.0f);
    }
    else if (selected)
    {
        g.setColour (Pal::surface);
        g.fillRoundedRectangle (r, 7.0f);
    }

    g.setColour (item.current ? Pal::accent : Pal::text);
    g.setFont (juce::FontOptions (15.0f, item.current ? juce::Font::bold : juce::Font::plain));
    g.drawText (item.text, r.reduced (12.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, true);
}

void PlayerBar::listBoxItemClicked (int, const juce::MouseEvent&) {}

void PlayerBar::rowDown (const juce::MouseEvent& e)
{
    pendingRow = rowAt (e);
    dragFrom = dragTo = -1;
}

std::vector<int> PlayerBar::getSelectedIds() const
{
    std::vector<int> ids;
    const auto& list_ = rows[(int) tab];
    auto sel = list.getSelectedRows();
    for (int i = 0; i < sel.size(); ++i)
    {
        const int r = sel[i];
        if (r >= 0 && r < (int) list_.size() && ! list_[(size_t) r].header)
            ids.push_back (list_[(size_t) r].id);
    }
    return ids;
}

int PlayerBar::rowAt (const juce::MouseEvent& e)
{
    const auto pos = e.getEventRelativeTo (&list).getPosition();
    return list.getRowContainingPosition (pos.x, juce::jlimit (0, list.getHeight() - 1, pos.y));
}

void PlayerBar::rowDrag (const juce::MouseEvent& e)
{
    const auto& list_ = rows[(int) tab];
    if (dragFrom >= 0 || pendingRow < 0 || pendingRow >= (int) list_.size() || list_.size() < 2)
        return;
    if (e.getDistanceFromDragStart() < 8)
        return;
    auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this);
    if (container == nullptr)
        return;

    // like dragging a plugin: a picture of the row follows the mouse
    const int row = pendingRow;
    const int w = list.getWidth(), h = list.getRowHeight();
    juce::Image image (juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g (image);
        g.fillAll (Pal::surfaceHi.withAlpha (0.95f));
        paintListBoxItem (row, g, w, h, true);
    }
    dragFrom = row;
    dragTo = row;
    pendingRow = -1;
    list.repaint();
    container->startDragging ("tcrow:" + juce::String ((int) tab) + ":" + juce::String (row), this, juce::ScaledImage (image), true);
}

int PlayerBar::rowForDragPosition (juce::Point<int> p) const
{
    const int n = (int) rows[(int) tab].size();
    const int scroll = list.getViewport() != nullptr ? list.getViewport()->getViewPositionY() : 0;
    return juce::jlimit (0, juce::jmax (0, n - 1), (p.y - list.getY() + scroll) / juce::jmax (1, list.getRowHeight()));
}

bool PlayerBar::isInterestedInDragSource (const SourceDetails& d)
{
    return d.description.toString().startsWith ("tcrow:" + juce::String ((int) tab) + ":");
}

void PlayerBar::itemDragEnter (const SourceDetails& d) { itemDragMove (d); }

void PlayerBar::itemDragMove (const SourceDetails& d)
{
    dragTo = rowForDragPosition (d.localPosition);
    list.scrollToEnsureRowIsOnscreen (dragTo);
    repaint();
}

void PlayerBar::itemDragExit (const SourceDetails&)
{
    dragTo = dragFrom;
    repaint();
}

void PlayerBar::itemDropped (const SourceDetails& d)
{
    const int from = d.description.toString().fromLastOccurrenceOf (":", false, false).getIntValue();
    const int to = rowForDragPosition (d.localPosition);
    const auto t = tab;
    dragFrom = dragTo = -1;
    list.repaint();
    repaint();
    if (from != to && cb.rowMoved)
        cb.rowMoved (t, from, to);
}

void PlayerBar::rowUp (const juce::MouseEvent& e)
{
    const bool extend = e.mods.isCtrlDown() || e.mods.isShiftDown() || e.mods.isCommandDown();
    const auto t = tab;
    const int click = pendingRow;
    pendingRow = -1;

    const auto& list_ = rows[(int) t];
    if (! extend && click >= 0 && click < (int) list_.size() && ! list_[(size_t) click].header && cb.rowClick)
        cb.rowClick (t, list_[(size_t) click].id);
}

void PlayerBar::paintOverChildren (juce::Graphics& g)
{
    if (dragFrom < 0 || dragTo < 0 || dragTo == dragFrom)
        return;
    const int rh = list.getRowHeight();
    const int scroll = list.getViewport() != nullptr ? list.getViewport()->getViewPositionY() : 0;
    const int y = list.getY() + (dragTo + (dragTo > dragFrom ? 1 : 0)) * rh - scroll;
    g.setColour (Pal::accent);
    g.fillRoundedRectangle ((float) list.getX() + 6.0f, (float) y - 1.5f, (float) list.getWidth() - 12.0f, 3.0f, 1.5f);
}

void PlayerBar::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    const auto& list_ = rows[(int) tab];
    if (row >= 0 && row < (int) list_.size() && ! list_[(size_t) row].header && cb.rowDouble)
        cb.rowDouble (tab, list_[(size_t) row].id);
}
