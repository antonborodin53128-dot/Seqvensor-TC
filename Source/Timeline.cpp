#include "Timeline.h"
#include "Text.h"

namespace
{
    void vline (juce::Graphics& g, float x, float y, float h, float thickness = 1.0f)
    {
        g.fillRect (x - thickness * 0.5f, y, thickness, h);
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
        g.setColour (juce::Colour (0xff0b0d10));
        g.fillRect (x0, (float) area.getY(), juce::jmax (0.0f, cx - x0), (float) area.getHeight());
        g.setColour (juce::Colour (0xff1a2423));
        g.fillRect (cx, (float) area.getY(), zx - cx, (float) area.getHeight());
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
        g.setColour (bar ? juce::Colour (0x59ffffff) : juce::Colour (0x1fffffff));
        vline (g, x, (float) area.getY() + (withNumbers && ! bar ? area.getHeight() * 0.55f : 0.0f),
               (float) area.getHeight() - (withNumbers && ! bar ? area.getHeight() * 0.55f : 0.0f));

        if (withNumbers && bar && barPx >= 22.0)
        {
            g.setColour (juce::Colour (0xffc7ccd3));
            g.drawText (juce::String (barNumber), (int) x + 4, area.getY() + 2, 40, 14, juce::Justification::left);
        }
    }

    // start of the music = bar 1
    g.setColour (juce::Colour (0xff2fb5a2));
    vline (g, zx, (float) area.getY(), (float) area.getHeight(), 2.0f);
    g.restoreState();
}

// ---------------------------------------------------------------- ruler

void RulerComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1b1f24));
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

    if (cb.filesDropped && ! audio.isEmpty())
        cb.filesDropped (track, audio, snapTime (view.xToTime ((float) x, (float) getWidth()), view.snap));
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

void LaneComponent::mouseDown (const juce::MouseEvent& e)
{
    dragClip = nullptr;
    panning = false;
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
        dragClip = clip;
        dragClipStart = clip->start.load();
    }
    else
    {
        panning = true;
        dragClock = view.clock;
    }
}

void LaneComponent::mouseDrag (const juce::MouseEvent& e)
{
    const double dt = (double) e.getDistanceFromDragStartX() / (double) juce::jmax (1, getWidth()) * view.span;

    if (dragClip != nullptr)
    {
        dragClip->start.store (snapTime (dragClipStart + dt, view.snap && ! e.mods.isAltDown()));
        engine.clipsChanged();
        if (cb.changed)
            cb.changed();
    }
    else if (panning && cb.seek)
    {
        cb.seek (dragClock - dt);
    }
}

void LaneComponent::mouseUp (const juce::MouseEvent&)
{
    dragClip = nullptr;
    panning = false;
}

void LaneComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    const float w = (float) bounds.getWidth();
    const double beat = engine.getBeatSeconds();

    g.fillAll (juce::Colour (0xff14171b));
    drawTimelineGrid (g, view, beat, bounds, false);

    for (auto& c : track->getClips())
    {
        const double start = c->start.load();
        const double len = c->data->lengthSeconds();
        const double tLeft = view.xToTime (0.0f, w), tRight = view.xToTime (w, w);
        const double end = c->loop.load() ? juce::jmax (tRight, start + len) : start + len;
        if (end < tLeft || start > tRight)
            continue;

        const int x0 = (int) std::floor (view.timeToX (start, w));
        const int x1 = (int) std::ceil (view.timeToX (end, w));
        const int cx0 = juce::jmax (0, x0), cx1 = juce::jmin (bounds.getWidth(), x1);
        if (cx1 <= cx0)
            continue;

        auto body = juce::Rectangle<int> (cx0, 3, cx1 - cx0, getHeight() - 6);
        g.setColour (juce::Colour (0xff1d3a4a));
        g.fillRect (body);

        // waveform of the visible part (repeated for looping clips)
        g.setColour (juce::Colour (0xff4fc3ff));
        if (! c->loop.load())
        {
            const double t0 = view.xToTime ((float) cx0, w) - start;
            const double t1 = view.xToTime ((float) cx1, w) - start;
            c->data->draw (g, body.reduced (0, 3), juce::jmax (0.0, t0), juce::jmin (len, t1));
        }
        else
        {
            for (double s = start; s < tRight; s += len)
            {
                const int a = juce::jmax (cx0, (int) std::floor (view.timeToX (s, w)));
                const int b = juce::jmin (cx1, (int) std::ceil (view.timeToX (s + len, w)));
                if (b > a)
                    c->data->draw (g, juce::Rectangle<int> (a, 6, b - a, getHeight() - 12),
                                   view.xToTime ((float) a, w) - s, view.xToTime ((float) b, w) - s);
            }
        }

        // the part before the count-in is never played
        const int dimEnd = (int) view.timeToX (-4.0 * beat, w);
        if (dimEnd > cx0)
        {
            g.setColour (juce::Colour (0xb0000000));
            g.fillRect (cx0, 3, juce::jmin (dimEnd, cx1) - cx0, getHeight() - 6);
        }

        g.setColour (juce::Colour (0xff4fc3ff));
        g.drawRect (body, 1);

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (c->data->name + (c->loop.load() ? "  (повтор)"_ru : juce::String()),
                    juce::jmax (x0, 0) + 5, 5, 400, 14, juce::Justification::left, false);
    }

    if (track->loadingClips.load() > 0)
    {
        g.setColour (juce::Colour (0xffe2b93b));
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Загрузка аудио..."_ru, 8, 0, 300, getHeight(), juce::Justification::centredLeft);
    }

    g.setColour (juce::Colour (0xff2a2f36));
    g.drawHorizontalLine (getHeight() - 1, 0.0f, w);

    if (dropHighlight)
    {
        g.setColour (juce::Colour (0x402fb5a2));
        g.fillRect (bounds);
        g.setColour (juce::Colour (0xff2fb5a2));
        g.drawRect (bounds, 2);
    }
}

// ---------------------------------------------------------------- playhead

void PlayheadOverlay::paint (juce::Graphics& g)
{
    const float w = (float) getWidth();
    const float x = (float) (view.playheadFraction * w);

    g.setColour (juce::Colour (0xffffd24d));
    g.fillRect (x - 1.0f, 0.0f, 2.0f, (float) getHeight());

    // current bar | beat
    const double beatPos = view.clock / engine.getBeatSeconds();
    const int bar = (int) std::floor (beatPos / 4.0) + 1;
    const int beat = (int) std::floor (std::fmod (std::fmod (beatPos, 4.0) + 4.0, 4.0)) + 1;
    const juce::String label = "Такт "_ru + juce::String (bar) + " | " + juce::String (beat);

    auto box = juce::Rectangle<float> (x + 4.0f, 3.0f, 86.0f, 18.0f);
    g.setColour (juce::Colour (0xe0000000));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (juce::Colour (0xffffd24d));
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText (label, box.toNearestInt(), juce::Justification::centred);
}
