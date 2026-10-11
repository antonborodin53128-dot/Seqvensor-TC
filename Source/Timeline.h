#pragma once

#include "Engine.h"

// Shared view of the timeline: the playhead line stays at a fixed x, the clips scroll under it.
struct TimelineView
{
    double span = 16.0;             // seconds visible across the lane width
    double playheadFraction = 0.25; // where the fixed line is (0..1 of the lane width)
    double clock = 0.0;             // song position in seconds (0 = bar 1, beat 1; < 0 = count-in)
    bool snap = true;               // clips snap to 1/16 of a beat unless Alt is held

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
        std::function<void (std::shared_ptr<Track>, std::shared_ptr<Clip>, juce::Point<int> screenPos)> clipMenu;
        std::function<void (std::shared_ptr<Track>, double time, juce::Point<int> screenPos)> laneMenu;
        std::function<void (std::shared_ptr<Track>, const juce::StringArray& files, double time)> filesDropped;
        std::function<void()> changed;   // clips moved: repaint everything
    };

    LaneComponent (Engine& e, std::shared_ptr<Track> t, TimelineView& v, Callbacks c)
        : engine (e), track (std::move (t)), view (v), cb (std::move (c)) {}

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override { if (cb.zoom) cb.zoom (w.deltaY); }

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dropHighlight = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dropHighlight = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    static bool isAudioFile (const juce::String& path);

private:
    std::shared_ptr<Clip> clipAt (float x) const;
    double snapTime (double t, bool allowSnap) const;

    Engine& engine;
    std::shared_ptr<Track> track;
    TimelineView& view;
    Callbacks cb;

    std::shared_ptr<Clip> dragClip;
    double dragClipStart = 0.0, dragClock = 0.0;
    bool panning = false, dropHighlight = false;
};

// Fixed playhead line over the ruler and all lanes + "bar | beat" readout.
class PlayheadOverlay : public juce::Component
{
public:
    PlayheadOverlay (Engine& e, TimelineView& v) : engine (e), view (v) { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics&) override;

private:
    Engine& engine;
    TimelineView& view;
};

// Draws vertical grid lines (beats / bars) and the count-in shading; shared by ruler and lanes.
void drawTimelineGrid (juce::Graphics& g, const TimelineView& view, double beatSeconds,
                       juce::Rectangle<int> area, bool withNumbers);
