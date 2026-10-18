#pragma once

#include "Engine.h"

#include <set>

// Shared view of the timeline: the playhead line stays at a fixed x, the clips scroll under it.
struct TimelineView
{
    double span = 16.0;             // seconds visible across the lane width
    double playheadFraction = 0.25; // where the fixed line is (0..1 of the lane width)
    double clock = 0.0;             // song position in seconds (0 = bar 1, beat 1; < 0 = count-in)
    bool snap = true;               // clips snap to 1/16 of a beat unless Alt is held
    std::set<int> selected;         // ids of the selected clips (all lanes)
    bool locked = false;            // the lock: nothing on the timeline / the tracks can be changed

    // Everything is pixel-locked: the scroll position is a whole number of pixels, so grid lines and waveform
    // columns keep exactly the same pixels while they slide (no shimmer / blur).
    double pxPerSec (float width) const { return (double) width / span; }
    int headX (float width) const { return (int) std::lround (playheadFraction * width); }
    double scrollPx (float width) const { return (double) std::llround (clock * pxPerSec (width)); }

    float timeToX (double t, float width) const { return (float) ((double) headX (width) + t * pxPerSec (width) - scrollPx (width)); }
    double xToTime (float x, float width) const { return ((double) x - headX (width) + scrollPx (width)) / pxPerSec (width); }
};

// Ruler with bar / beat numbers. Drag = move the song position, wheel = zoom.
class RulerComponent : public juce::Component
{
public:
    RulerComponent (Engine& e, TimelineView& v) : engine (e), view (v) {}

    std::function<void (double newClock)> onSeek;
    std::function<void (float wheelDelta)> onZoom;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { dragClock = view.clock; }
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override { if (onZoom) onZoom (w.deltaY); }

private:
    Engine& engine;
    TimelineView& view;
    double dragClock = 0.0;
};

// The audio lane of one track: clips, grid, drag & drop of files.
class LaneComponent : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    struct Callbacks
    {
        std::function<void (double newClock)> seek;
        std::function<void (float wheelDelta)> zoom;
        // selection: pressing a clip (ctrl = toggle, shift = range), a plain click on an already selected clip
        std::function<void (std::shared_ptr<Track>, std::shared_ptr<Clip>, const juce::ModifierKeys&)> clipPressed;
        std::function<void (std::shared_ptr<Clip>)> clipClickedAlone;
        std::function<void (std::shared_ptr<Clip> primary, double deltaSeconds)> dragSelected;   // move all selected clips
        std::function<void (const juce::ModifierKeys&)> emptyPressed;                            // click on an empty place
        std::function<void (juce::Rectangle<int> screenRect, bool finished)> marquee;             // rubber band
        std::function<void()> focus;                                                              // keyboard focus for Delete / Ctrl+A
        std::function<void (std::shared_ptr<Track>, std::shared_ptr<Clip>, juce::Point<int> screenPos)> clipMenu;
        std::function<void (std::shared_ptr<Track>, double time, juce::Point<int> screenPos)> laneMenu;
        std::function<void (std::shared_ptr<Track>, const juce::StringArray& files, double time)> filesDropped;
        std::function<void()> changed;   // clips moved: repaint everything
        std::function<void (std::shared_ptr<Track>, std::shared_ptr<Clip>, double time)> cutClip;   // Alt + click
        std::function<void()> trimmed;                                                              // an edge was dragged
    };

    LaneComponent (Engine& e, std::shared_ptr<Track> t, TimelineView& v, Callbacks c)
        : engine (e), track (std::move (t)), view (v), cb (std::move (c)) {}

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void modifierKeysChanged (const juce::ModifierKeys&) override { repaint(); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override { if (cb.zoom) cb.zoom (w.deltaY); }

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dropHighlight = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dropHighlight = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    static bool isAudioFile (const juce::String& path);

    // ids of the clips of this lane that touch a rectangle given in screen coordinates
    void collectClipsInScreenRect (juce::Rectangle<int> screenRect, std::set<int>& ids) const;

private:
    std::shared_ptr<Clip> clipAt (float x) const;
    // a clip edge under x (within a few pixels): side -1 = start, +1 = end
    std::shared_ptr<Clip> edgeAt (float x, int& side) const;
    double snapTime (double t, bool allowSnap) const;

    Engine& engine;
    std::shared_ptr<Track> track;
    TimelineView& view;
    Callbacks cb;

    std::shared_ptr<Clip> dragClip;
    double dragClipStart = 0.0;
    bool didDrag = false, pressedOnSelected = false, plainPress = false, marqueeing = false, dropHighlight = false;
    std::shared_ptr<Clip> trimClip;
    int trimSide = 0;
    float hoverX = -1.0f;
};

// Fixed playhead line over the ruler and all lanes + "bar | beat" readout.
class PlayheadOverlay : public juce::Component
{
public:
    PlayheadOverlay (Engine& e, TimelineView& v) : engine (e), view (v) { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics&) override;

    void setMarquee (juce::Rectangle<int> r) { if (r != marquee) { marquee = r; repaint(); } }

private:
    Engine& engine;
    TimelineView& view;
    juce::Rectangle<int> marquee;
};

// Elapsed / remaining time of the playing composition (top bar).
class TimeReadout : public juce::Component
{
public:
    void setTimes (double elapsedSeconds, double remainingSeconds);
    void paint (juce::Graphics&) override;

private:
    static juce::String format (double seconds);
    juce::String elapsedText { "0:00" }, remainingText { "-0:00" };
};

// Overview strip above the ruler: the whole song, the visible part as a draggable window, the playhead.
// Drag the window / click / wheel = scroll the timeline.
class TimelineScroller : public juce::Component
{
public:
    TimelineScroller (Engine& e, TimelineView& v) : engine (e), view (v) { setMouseCursor (juce::MouseCursor::DraggingHandCursor); }

    std::function<void (double newClock)> onSeek;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    double rangeStart() const;
    double rangeEnd() const;
    float timeToX (double t) const;
    double xToTime (float x) const;
    juce::Rectangle<float> windowRect() const;

    Engine& engine;
    TimelineView& view;
    double dragClock0 = 0.0;
};

// Draws vertical grid lines (beats / bars) and the count-in shading; shared by ruler and lanes.
void drawTimelineGrid (juce::Graphics& g, const TimelineView& view, double beatSeconds,
                       juce::Rectangle<int> area, bool withNumbers);
