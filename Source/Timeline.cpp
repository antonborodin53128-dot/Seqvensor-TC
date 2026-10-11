#include "Timeline.h"
#include "Text.h"
#include "Style.h"

namespace
{
    void vline (juce::Graphics& g, float x, float y, float h, int thickness = 1)
    {
        g.fillRect ((int) std::lround (x) - thickness / 2, (int) std::lround (y), thickness, (int) std::lround (h));
    }
}

void drawTimelineGrid (juce::Graphics& g, const TimelineView& view, double beat,
                       juce::Rectangle<int> area, bool withNumbers)
{
    const float w = (float) area.getWidth();
    const float x0 = (float) area.getX();
    const float pxPerSec = w / (float) view.span;
    const double beatPx = beat * pxPerSec;
    const double barPx = beatPx * 4.0;

    // count-in bar (the 4 beats before bar 1) and the part before it, which is never played
    const float cx = x0 + view.timeToX (-4.0 * beat, w);
    const float zx = x0 + view.timeToX (0.0, w);
    {
        g.saveState();
        g.reduceClipRegion (area);
        g.setColour (juce::Colour (0xff000000));
        g.fillRect ((int) x0, area.getY(), juce::jmax (0, (int) std::lround (cx - x0)), area.getHeight());
        g.setColour (juce::Colour (0xff0c1a14));
        g.fillRect ((int) std::lround (cx), area.getY(), (int) std::lround (zx) - (int) std::lround (cx), area.getHeight());
        g.restoreState();
    }

    const double tLeft = view.xToTime (0.0f, w), tRight = view.xToTime (w, w);
    const int firstBeat = (int) std::floor (tLeft / beat);
    const int lastBeat = (int) std::ceil (tRight / beat);
    g.saveState();
    g.reduceClipRegion (area);
    g.setFont (juce::FontOptions (11.0f));

    const int barStep = barPx >= 28.0 ? 1 : juce::jmax (1, (int) std::ceil (28.0 / juce::jmax (1.0, barPx)));
    for (int k = firstBeat; k <= lastBeat; ++k)
    {
        const bool bar = (((k % 4) + 4) % 4) == 0;
        const int barNumber = (int) std::floor ((double) k / 4.0) + 1;
        if (! bar && beatPx < 9.0)
            continue;
        if (bar && barStep > 1 && (barNumber % barStep) != 0 && barNumber != 1)
            continue;

        const float x = x0 + view.timeToX ((double) k * beat, w);
        g.setColour (bar ? juce::Colour (0x40ffffff) : juce::Colour (0x17ffffff));
        vline (g, x, (float) area.getY() + (withNumbers && ! bar ? area.getHeight() * 0.55f : 0.0f),
               (float) area.getHeight() - (withNumbers && ! bar ? area.getHeight() * 0.55f : 0.0f));

        if (withNumbers && bar && barPx >= 22.0)
        {
            g.setColour (Pal::textDim);
            g.drawText (juce::String (barNumber), (int) x + 4, area.getY() + 2, 40, 14, juce::Justification::left);
        }
    }

    // start of the music = bar 1
    g.setColour (Pal::green.withAlpha (0.9f));
    vline (g, zx, (float) area.getY(), (float) area.getHeight(), 2);
    g.restoreState();
}

// ---------------------------------------------------------------- ruler

void RulerComponent::paint (juce::Graphics& g)
{
    g.fillAll (Pal::surfaceLo);
    drawTimelineGrid (g, view, engine.getBeatSeconds(), getLocalBounds(), true);
}

void RulerComponent::mouseDrag (const juce::MouseEvent& e)
{
    // dragging moves the timeline under the fixed playhead line
    const double dt = (double) e.getDistanceFromDragStartX() / (double) juce::jmax (1, getWidth()) * view.span;
    if (onSeek)
        onSeek (dragClock - dt);
}

// ---------------------------------------------------------------- lane

bool LaneComponent::isAudioFile (const juce::String& path)
{
    const auto ext = juce::File (path).getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".mp3" || ext == ".ogg" || ext == ".wave";
}

bool LaneComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (isAudioFile (f))
            return true;
    return false;
}

double LaneComponent::snapTime (double t, bool allowSnap) const
{
    const double beat = engine.getBeatSeconds();
    if (allowSnap)
    {
        const double step = beat / 4.0;
        t = std::round (t / step) * step;
    }
    return juce::jmax (t, -4.0 * beat);   // at most one bar (the count-in) before bar 1
}

void LaneComponent::filesDropped (const juce::StringArray& files, int x, int)
{
    dropHighlight = false;
    repaint();

    juce::StringArray audio;
    for (auto& f : files)
        if (isAudioFile (f))
            audio.add (f);

    // dropped audio always lands on bar 1
    juce::ignoreUnused (x);
    if (cb.filesDropped && ! audio.isEmpty())
        cb.filesDropped (track, audio, 0.0);
}

std::shared_ptr<Clip> LaneComponent::clipAt (float x) const
{
    const float w = (float) getWidth();
    const auto& clips = track->getClips();
    for (size_t i = clips.size(); i-- > 0;)
    {
        const auto& c = clips[i];
        const float x0 = view.timeToX (c->start.load(), w);
        const float x1 = c->loop.load() ? w : view.timeToX (c->end(), w);
        if (x >= x0 && x <= x1)
            return c;
    }
    return nullptr;
}

void LaneComponent::collectClipsInScreenRect (juce::Rectangle<int> screenRect, std::set<int>& ids) const
{
    const auto local = screenRect.withPosition (getLocalPoint (nullptr, screenRect.getPosition()));
    const float w = (float) getWidth();
    if (local.getBottom() < 4 || local.getY() > getHeight() - 4)
        return;

    for (auto& c : track->getClips())
    {
        const float x0 = view.timeToX (c->start.load(), w);
        const float x1 = c->loop.load() ? w : view.timeToX (c->end(), w);
        if (x1 >= (float) local.getX() && x0 <= (float) local.getRight())
            ids.insert (c->id);
    }
}

void LaneComponent::mouseDown (const juce::MouseEvent& e)
{
    dragClip = nullptr;
    didDrag = false;
    marqueeing = false;
    plainPress = false;
    pressedOnSelected = false;
    if (cb.focus)
        cb.focus();

    auto clip = clipAt ((float) e.x);

    if (e.mods.isPopupMenu())
    {
        if (clip != nullptr)
        {
            if (cb.clipMenu)
                cb.clipMenu (track, clip, e.getScreenPosition());
        }
        else if (cb.laneMenu)
            cb.laneMenu (track, snapTime (view.xToTime ((float) e.x, (float) getWidth()), view.snap), e.getScreenPosition());
        return;
    }

    if (clip != nullptr)
    {
        pressedOnSelected = view.selected.count (clip->id) > 0;
        plainPress = ! e.mods.isShiftDown() && ! e.mods.isCtrlDown() && ! e.mods.isCommandDown();
        if (cb.clipPressed)
            cb.clipPressed (track, clip, e.mods);

        if (plainPress)
        {
            dragClip = clip;
            dragClipStart = clip->start.load();
        }
    }
    else
    {
        marqueeing = true;
        if (cb.emptyPressed)
            cb.emptyPressed (e.mods);
    }
}

void LaneComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() < 3 && ! didDrag)
        return;
    didDrag = true;

    if (dragClip != nullptr)
    {
        const double dt = (double) e.getDistanceFromDragStartX() / (double) juce::jmax (1, getWidth()) * view.span;
        const double newStart = snapTime (dragClipStart + dt, view.snap && ! e.mods.isAltDown());
        if (cb.dragSelected)
            cb.dragSelected (dragClip, newStart - dragClipStart);
    }
    else if (marqueeing && cb.marquee)
    {
        auto a = e.getMouseDownScreenPosition();
        auto b = e.getScreenPosition();
        cb.marquee (juce::Rectangle<int>::leftTopRightBottom (juce::jmin (a.x, b.x), juce::jmin (a.y, b.y),
                                                              juce::jmax (a.x, b.x), juce::jmax (a.y, b.y)), false);
    }
}

void LaneComponent::mouseUp (const juce::MouseEvent&)
{
    if (marqueeing && cb.marquee)
        cb.marquee ({}, true);

    // a plain click (no drag) on a clip of a multiple selection selects just that clip
    if (dragClip != nullptr && ! didDrag && pressedOnSelected && plainPress && view.selected.size() > 1 && cb.clipClickedAlone)
        cb.clipClickedAlone (dragClip);

    dragClip = nullptr;
    marqueeing = false;
    didDrag = false;
}

void LaneComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    const float w = (float) bounds.getWidth();
    const double beat = engine.getBeatSeconds();

    g.fillAll (Pal::sunken);
    drawTimelineGrid (g, view, beat, bounds, false);

    for (auto& c : track->getClips())
    {
        const double start = c->start.load();
        const double len = c->data->lengthSeconds();
        const double tLeft = view.xToTime (0.0f, w), tRight = view.xToTime (w, w);
        const double end = c->loop.load() ? juce::jmax (tRight, start + len) : start + len;
        if (end < tLeft || start > tRight)
            continue;

        const int x0 = (int) std::lround (view.timeToX (start, w));
        const int x1 = (int) std::lround (view.timeToX (end, w));
        const int cx0 = juce::jmax (0, x0), cx1 = juce::jmin (bounds.getWidth(), x1);
        if (cx1 <= cx0)
            continue;

        // rounded body (its outer corners stay off-screen when the clip is cut by the lane edge)
        auto body = juce::Rectangle<float> ((float) juce::jmax (x0, -20), 4.0f,
                                            (float) (juce::jmin (x1, bounds.getWidth() + 20) - juce::jmax (x0, -20)),
                                            (float) getHeight() - 8.0f);
        const bool isSelected = view.selected.count (c->id) > 0;
        g.setColour (isSelected ? Pal::clipBody.brighter (0.35f) : Pal::clipBody);
        g.fillRoundedRectangle (body, 8.0f);

        // waveform: columns are locked to absolute pixels, so scrolling only shifts them
        const double secPerPx = 1.0 / view.pxPerSec (w);
        auto drawWave = [&] (int xa, int xb, double origin)
        {
            if (xb <= xa)
                return;
            const double t0 = view.xToTime ((float) xa, w) - origin;
            c->data->draw (g, juce::Rectangle<int> (xa, 4 + 20, xb - xa, getHeight() - 8 - 24), t0, t0 + (xb - xa) * secPerPx);
        };

        g.setColour (Pal::clip);
        if (! c->loop.load())
            drawWave (cx0, cx1, start);
        else
            for (double s = start; s < tRight; s += len)
                drawWave (juce::jmax (cx0, (int) std::lround (view.timeToX (s, w))),
                          juce::jmin (cx1, (int) std::lround (view.timeToX (s + len, w))), s);

        // the part before the count-in is never played
        const int dimEnd = (int) std::lround (view.timeToX (-4.0 * beat, w));
        if (dimEnd > cx0)
        {
            g.setColour (juce::Colour (0xb0101012));
            g.fillRect (cx0, 4, juce::jmin (dimEnd, cx1) - cx0, getHeight() - 8);
        }

        if (isSelected)
        {
            g.setColour (Pal::accent);
            g.drawRoundedRectangle (body.reduced (1.0f), 8.0f, 2.0f);
        }
        else
        {
            g.setColour (Pal::clip.withAlpha (0.6f));
            g.drawRoundedRectangle (body.reduced (0.5f), 8.0f, 1.0f);
        }

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        g.drawText (c->data->name + (c->loop.load() ? "  (повтор)"_ru : juce::String()),
                    juce::jmax (x0, 0) + 10, 7, 400, 16, juce::Justification::left, false);
    }

    if (track->loadingClips.load() > 0)
    {
        g.setColour (Pal::orange);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Загрузка аудио..."_ru, 8, 0, 300, getHeight(), juce::Justification::centredLeft);
    }

    g.setColour (Pal::hairline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, w);

    if (dropHighlight)
    {
        g.setColour (Pal::accent.withAlpha (0.18f));
        g.fillRect (bounds);
        g.setColour (Pal::accent);
        g.drawRect (bounds, 2);
    }
}

// ---------------------------------------------------------------- playhead

void PlayheadOverlay::paint (juce::Graphics& g)
{
    const float w = (float) getWidth();
    const int x = view.headX (w);

    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.fillRect (x - 1, 0, 2, getHeight());

    // current bar | beat
    const double beatPos = view.clock / engine.getBeatSeconds();
    const int bar = (int) std::floor (beatPos / 4.0) + 1;
    const int beat = (int) std::floor (std::fmod (std::fmod (beatPos, 4.0) + 4.0, 4.0)) + 1;
    const juce::String label = "Такт "_ru + juce::String (bar) + " | " + juce::String (beat);

    auto box = juce::Rectangle<float> ((float) x + 6.0f, 4.0f, 92.0f, 20.0f);
    g.setColour (Pal::accent);
    g.fillRoundedRectangle (box, 7.0f);
    g.setColour (Pal::onAccent);
    g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    g.drawText (label, box.toNearestInt(), juce::Justification::centred);

    if (! marquee.isEmpty())
    {
        g.setColour (Pal::accent.withAlpha (0.14f));
        g.fillRect (marquee);
        g.setColour (Pal::accent.withAlpha (0.9f));
        g.drawRect (marquee, 1);
    }
}
