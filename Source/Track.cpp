#include "Track.h"
#include "LiveStretch.h"

#include <algorithm>
#include "Text.h"

Track::Track (juce::String trackName) : name (std::move (trackName))
{
}

Track::~Track() = default;

// ---------------------------------------------------------------- chain

void Track::setPlugin (int index, std::unique_ptr<juce::AudioPluginInstance> plugin,
                       const juce::PluginDescription& description)
{
    if (plugin == nullptr || ! juce::isPositiveAndBelow (index, numSlots))
        return;

    auto slot = std::make_unique<Slot>();
    slot->description = description;
    slot->plugin = std::move (plugin);

    std::unique_ptr<Slot> old;
    {
        const juce::ScopedLock sl (chainLock);
        old = std::move (slots[(size_t) index]);
        slots[(size_t) index] = std::move (slot);
    }

    if (old != nullptr && old->plugin != nullptr)
    {
        old->plugin->releaseResources();
        old->plugin->setPlayHead (nullptr);
    }
    refreshLatency();
}

void Track::moveSlot (int from, int to)
{
    if (! juce::isPositiveAndBelow (from, numSlots) || ! juce::isPositiveAndBelow (to, numSlots) || from == to)
        return;

    {
        const juce::ScopedLock sl (chainLock);
        if (from < to)
            std::rotate (slots.begin() + from, slots.begin() + from + 1, slots.begin() + to + 1);
        else
            std::rotate (slots.begin() + to, slots.begin() + from, slots.begin() + from + 1);
    }
    refreshLatency();
}

void Track::swapSlotWith (int index, Track& other, int otherIndex)
{
    if (! juce::isPositiveAndBelow (index, numSlots) || ! juce::isPositiveAndBelow (otherIndex, numSlots))
        return;

    if (&other == this)
    {
        moveSlot (index, otherIndex);
        return;
    }

    {
        Track* first = this < &other ? this : &other;     // fixed lock order
        Track* second = this < &other ? &other : this;
        const juce::ScopedLock a (first->chainLock);
        const juce::ScopedLock b (second->chainLock);
        std::swap (slots[(size_t) index], other.slots[(size_t) otherIndex]);
    }
    refreshLatency();
    other.refreshLatency();
    conformLayouts();
    other.conformLayouts();
}

void Track::setMono (bool on)
{
    if (! on && outputChannel.load() >= 0)
    {
        // a single output channel becomes the pair it belongs to
        if (outputPair.load() < 0)
            outputPair = outputChannel.load() / 2;
        outputChannel = -1;
    }
    relayout (on);
}

void Track::conformLayouts()
{
    relayout (mono.load());
}

void Track::relayout (bool toMono)
{
    const int wanted = toMono ? 1 : 2;
    bool needed = toMono != mono.load();
    for (auto& s : slots)
        if (s != nullptr && s->plugin != nullptr && s->requestedChannels != wanted)
            needed = true;
    if (! needed)
        return;

    // fade the audio out (a few blocks), change the layouts while nothing is audible, fade back in
    switching.store (true);
    if (juce::Time::getMillisecondCounter() - (juce::uint32) lastChunkMs.load() < 200)
        juce::Thread::sleep (45);

    {
        const juce::ScopedLock sl (chainLock);
        for (auto& s : slots)
        {
            if (s == nullptr || s->plugin == nullptr)
                continue;

            auto& p = *s->plugin;
            auto layout = p.getBusesLayout();
            const auto channelSet = toMono ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
            if (p.getBusCount (true) > 0 && p.getBus (true, 0)->isNumberOfChannelsSupported (wanted))
                layout.inputBuses.getReference (0) = channelSet;
            if (p.getBusCount (false) > 0 && p.getBus (false, 0)->isNumberOfChannelsSupported (wanted))
                layout.outputBuses.getReference (0) = channelSet;

            p.releaseResources();
            p.setBusesLayout (layout);     // refused = the plugin stays as it was (the fallback path in runChain)
            p.setRateAndBufferSizeDetails (sampleRate, maxBlock);
            p.prepareToPlay (sampleRate, maxBlock);
            s->requestedChannels = wanted;
        }
        mono.store (toMono);
    }

    refreshLatency();
    switching.store (false);
}

void Track::clearSlot (int index)
{
    if (! juce::isPositiveAndBelow (index, numSlots))
        return;

    std::unique_ptr<Slot> old;
    {
        const juce::ScopedLock sl (chainLock);
        old = std::move (slots[(size_t) index]);
    }

    if (old == nullptr)
        return;

    if (old->plugin != nullptr)
    {
        old->plugin->releaseResources();
        old->plugin->setPlayHead (nullptr);
    }
    refreshLatency();
}

int Track::getNumPlugins() const
{
    int n = 0;
    for (auto& s : slots)
        if (s != nullptr && s->plugin != nullptr)
            ++n;
    return n;
}

Track::Slot* Track::getSlot (int index) const
{
    return juce::isPositiveAndBelow (index, numSlots) ? slots[(size_t) index].get() : nullptr;
}

int Track::findFreeSlot() const
{
    for (int i = 0; i < numSlots; ++i)
        if (slots[(size_t) i] == nullptr)
            return i;
    return -1;
}

int Track::computeLatencySamples() const
{
    int total = 0;
    for (auto& s : slots)
        if (s != nullptr && s->plugin != nullptr && ! s->bypass.load())
            total += s->plugin->getLatencySamples();
    return total;
}

// ---------------------------------------------------------------- clips

std::shared_ptr<Clip> Track::addClip (std::shared_ptr<ClipData> data, double startSeconds, int clipId,
                                      std::shared_ptr<LiveStretch> live)
{
    auto clip = std::make_shared<Clip>();
    clip->id = clipId;
    clip->original = data;
    clip->data = std::move (data);
    clip->live = std::move (live);
    clip->start.store (startSeconds);

    {
        const juce::SpinLock::ScopedLockType sl (clipLock);
        clips.push_back (clip);
    }
    return clip;
}

void Track::replaceClipData (const std::shared_ptr<Clip>& clip, std::shared_ptr<ClipData> newData, double clockNow)
{
    {
        const juce::SpinLock::ScopedLockType sl (clipLock);

        const double oldLen = clip->data->lengthSeconds(), newLen = newData->lengthSeconds();
        const double rel = clockNow - clip->start.load();            // time since the clip's start on the timeline
        double o = rel + clip->shift.load();                         // where the audio is right now
        if (o < 0.0)
            clip->shift.store (0.0);                                 // not started yet: starts on its bar
        else if (oldLen > 0.0 && newLen > 0.0 && (clip->loop.load() || o < oldLen))
        {
            if (clip->loop.load())
                o = std::fmod (o, oldLen);
            clip->shift.store (o * newLen / oldLen - rel);           // same musical point in the new audio
        }
        std::swap (clip->data, newData);
    }
    // newData now holds the previous audio and is released here, outside the lock and off the audio thread
}

void Track::setLive (const std::shared_ptr<Clip>& clip, std::shared_ptr<LiveStretch> live)
{
    {
        const juce::SpinLock::ScopedLockType sl (clipLock);
        std::swap (clip->live, live);
    }
    // the previous stretcher is released here, outside the lock and off the audio thread
}

void Track::retempo (double r, double clockNow, double newBpm)
{
    const juce::SpinLock::ScopedLockType sl (clipLock);
    for (auto& c : clips)
    {
        const double s = c->start.load();
        if (c->follows())
        {
            // a following clip is positioned from the bars alone: nothing to keep continuous
            c->shift.store (0.0);
        }
        else
        {
            const double sh = c->shift.load();
            const double o = (clockNow - s) + sh;
            // audio that is sounding keeps its place; a clip that has not started yet will start on its bar
            c->shift.store (o < 0.0 ? 0.0 : sh + (clockNow - s) * (1.0 - r));
        }
        c->start.store (s * r);
        c->updateSpeed (newBpm);
    }
}

void Track::clearShifts()
{
    const juce::SpinLock::ScopedLockType sl (clipLock);
    for (auto& c : clips)
        c->shift.store (0.0);
}

void Track::removeClip (int clipId)
{
    std::shared_ptr<Clip> removed;
    {
        const juce::SpinLock::ScopedLockType sl (clipLock);
        for (size_t i = 0; i < clips.size(); ++i)
            if (clips[i]->id == clipId)
            {
                removed = clips[i];
                clips.erase (clips.begin() + (long) i);
                break;
            }
    }
}

double Track::getEndSeconds() const
{
    double end = 0.0;
    for (auto& c : clips)
        if (! c->loop.load())
            end = juce::jmax (end, c->end());
    return end;
}

double Track::getEarliestStart() const
{
    double first = 0.0;
    for (auto& c : clips)
        first = juce::jmin (first, c->visibleStart());
    return first;
}

// ---------------------------------------------------------------- audio

void Track::prepare (double newSampleRate, int maxBlockSize, juce::AudioPlayHead* playHead)
{
    sampleRate = newSampleRate;
    maxBlock = juce::jmax (64, maxBlockSize);

    stereoBuf.setSize (2, maxBlock, false, true, false);
    busOut.setSize (2, maxBlock, false, true, false);
    clipScratch.setSize (2, maxBlock, false, true, false);
    clickBuf.setSize (2, maxBlock, false, true, false);
    pluginBuf.setSize (16, maxBlock, false, true, false);
    midi.ensureSize (2048);

    const int maxDelay = (int) (sampleRate * 2.0) + maxBlock;
    delayBuf.setSize (2, maxDelay, false, true, false);
    delayWritePos = 0;

    const juce::ScopedLock sl (chainLock);
    for (auto& s : slots)
    {
        if (s == nullptr || s->plugin == nullptr)
            continue;
        s->plugin->setPlayHead (playHead);
        s->plugin->setRateAndBufferSizeDetails (sampleRate, maxBlock);
        s->plugin->prepareToPlay (sampleRate, maxBlock);
    }
    refreshLatency();
}

void Track::release()
{
    const juce::ScopedLock sl (chainLock);
    for (auto& s : slots)
        if (s != nullptr && s->plugin != nullptr)
            s->plugin->releaseResources();
}

void Track::renderClick (float* l, float* r, int n)
{
    if (clickPos < 0)
        return;

    const int len = juce::jmax (1, (int) (sampleRate * 0.04));
    const double freq = clickAccent ? 1600.0 : 1000.0;
    const double w = juce::MathConstants<double>::twoPi * freq / sampleRate;

    for (int i = 0; i < n; ++i)
    {
        if (clickPos >= len)
        {
            clickPos = -1;
            break;
        }
        float env = 1.0f - (float) clickPos / (float) len;
        env *= env;
        const float s = 0.6f * env * (float) std::sin (w * (double) clickPos);
        l[i] += s;
        r[i] += s;
        ++clickPos;
    }
}

static inline float cubicSample (const float* x, int idx, float f, int count)
{
    auto at = [x, count] (int i) { return x[juce::jlimit (0, count - 1, i)]; };
    const float y0 = at (idx - 1), y1 = at (idx), y2 = at (idx + 1), y3 = at (idx + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

// Adds the audio clips that lie under the clock into stereoBuf (sample accurate, any clock jump is fine).
void Track::renderClips (int n, const ChunkClock& clock)
{
    if (! clock.running)
        return;

    const juce::SpinLock::ScopedTryLockType lock (clipLock);
    if (! lock.isLocked())
        return;

    auto* l = stereoBuf.getWritePointer (0);
    auto* r = stereoBuf.getWritePointer (1);

    const double bpm = bpmSource != nullptr ? bpmSource->load() : 120.0;

    for (auto& c : clips)
    {
        // A clip that follows the project tempo plays its original through the live stretcher: where it is in
        // the file follows from the position in bars, so it cannot drift from the metronome.
        const bool follow = c->follows();
        const double semis = isTransposed() ? (double) pitchSource->load() : 0.0;

        // rho = file seconds per project second. A clip that does not follow the tempo plays at the file's own
        // speed (a stretched copy lasts shorter or longer than the original: that ratio is its rho).
        double rho = 1.0;
        if (follow)
            rho = bpm / c->original->fileBpm;
        else if (semis != 0.0 && c->original != nullptr && c->data != nullptr && c->data != c->original && c->data->numSamples > 0)
            rho = c->original->lengthSeconds() / c->data->lengthSeconds();
        const bool rhoOk = rho >= 0.25 && rho <= 4.0;
        const bool needLive = rhoOk && (semis != 0.0 || (follow && std::abs (rho - 1.0) > 0.0005));
        const bool useLive = needLive && c->live != nullptr && c->live->isReady (sampleRate);

        // speed actually played: the stretched one, or the file's own if the stretcher is not available
        const bool liveBase = follow || (semis != 0.0 && c->data != c->original && c->original != nullptr);
        const double sp = follow ? ((useLive || (rhoOk && std::abs (rho - 1.0) <= 0.0005)) ? rho : 1.0)
                                 : (useLive ? rho : 1.0);

        const ClipData& d = (follow || (useLive && liveBase)) ? *c->original : *c->data;
        const double start = c->start.load() - (follow ? 0.0 : c->shift.load());
        const double len = d.lengthSeconds() / sp;
        const bool loop = c->loop.load();
        const double t0 = clock.startSeconds;
        const double fin = loop ? 0.0 : juce::jlimit (0.0, 1.0, c->trimIn.load());
        const double fout = loop ? 1.0 : juce::jlimit (fin, 1.0, 1.0 - c->trimOut.load());
        const double vs = start + fin * len, ve = start + fout * len;   // the visible (played) part on the timeline
        // a trimmed edge is played 1.5 ms further with a 3 ms linear fade centred on it: the two halves of a cut clip
        // overlap and add up to exactly the uncut sound, a trimmed edge simply fades
        const double halfFade = 0.0015;
        const double vsPlay = fin > 0.0 ? vs - halfFade : vs, vePlay = fout < 1.0 ? ve + halfFade : ve;

        const int i0 = juce::jmax (0, (int) std::ceil ((vsPlay - t0) * sampleRate));
        const int i1 = loop ? n : juce::jmin (n, (int) std::ceil ((vePlay - t0) * sampleRate));
        if (i0 >= i1 || len <= 0.0)
        {
            if (c->live != nullptr)
                c->live->idle();
            continue;
        }

        // a trimmed edge gets a 3 ms fade, so a cut never clicks; the clip is rendered into a scratch buffer first
        const bool edges = fin > 0.0 || fout < 1.0;
        float* outL = l;
        float* outR = r;
        if (edges)
        {
            clipScratch.clear (0, n);
            outL = clipScratch.getWritePointer (0);
            outR = clipScratch.getWritePointer (1);
        }
        auto addEdges = [&]
        {
            if (! edges)
                return;
            const double fade = 0.003;
            for (int i = i0; i < i1; ++i)
            {
                const double tl = t0 + (double) i / sampleRate;
                float g = 1.0f;
                if (fin > 0.0)
                    g *= (float) juce::jlimit (0.0, 1.0, (tl - (vs - halfFade)) / fade);
                if (fout < 1.0)
                    g *= (float) juce::jlimit (0.0, 1.0, ((ve + halfFade) - tl) / fade);
                l[i] += outL[i] * g;
                r[i] += outR[i] * g;
            }
        };

        if (useLive)
        {
            const double posStart = (t0 + (double) i0 / sampleRate - start) * rho * sampleRate;   // file samples
            c->live->render (outL + i0, outR + i0, i1 - i0, posStart, rho, loop, semis);
            addEdges();
            continue;
        }
        if (c->live != nullptr)
            c->live->idle();

        const float* L = d.audio.getReadPointer (0);
        const float* R = d.audio.getNumChannels() > 1 ? d.audio.getReadPointer (1) : L;
        const double ratio = d.sampleRate / sampleRate;
        double pos = (t0 + (double) i0 / sampleRate - start) * sp * d.sampleRate;
        const double total = (double) d.numSamples;

        for (int i = i0; i < i1; ++i)
        {
            if (loop && pos >= total)
                pos = std::fmod (pos, total);

            const int idx = (int) pos;
            if (idx >= 0 && idx < d.numSamples)
            {
                if (ratio == 1.0)
                {
                    outL[i] += L[idx];
                    outR[i] += R[idx];
                }
                else
                {
                    const float f = (float) (pos - idx);
                    outL[i] += cubicSample (L, idx, f, d.numSamples);
                    outR[i] += cubicSample (R, idx, f, d.numSamples);
                }
            }
            pos += ratio;
        }
        addEdges();
    }
}

// Metronome clicks of this track, from the engine's global clock events.
void Track::renderClicks (int n, const ChunkClock& clock)
{
    clickBuf.clear (0, n);
    auto* cl = clickBuf.getWritePointer (0);
    auto* cr = clickBuf.getWritePointer (1);

    if (clock.running && metronomeOn.load())
    {
        int pos = 0;
        for (int e = 0; e < clock.numEvents; ++e)
        {
            const int at = juce::jlimit (pos, n, clock.eventOffset[e]);
            renderClick (cl + pos, cr + pos, at - pos);
            pos = at;
            clickPos = 0;
            clickAccent = clock.eventAccent[e];
        }
        renderClick (cl + pos, cr + pos, n - pos);
    }
    else
    {
        clickPos = -1;
    }
}

void Track::processChunk (const float* const* inputs, int numInputs,
                          float* const* outputs, int numOutputs, int offset, int n,
                          const Track* busSource, const ChunkClock& clock)
{
    lastChunkMs.store ((juce::int64) juce::Time::getMillisecondCounter());
    const bool isMono = mono.load();

    auto* l = stereoBuf.getWritePointer (0);
    auto* r = stereoBuf.getWritePointer (1);

    if (busSource != nullptr && busSource != this && busSource->busOut.getNumSamples() >= n)
    {
        juce::FloatVectorOperations::copy (l, busSource->busOut.getReadPointer (0), n);
        juce::FloatVectorOperations::copy (r, busSource->busOut.getReadPointer (1), n);
    }
    else
    {
        const int il = inputL.load();
        const int ir = inputR.load();
        const bool hasL = il >= 0 && il < numInputs && inputs[il] != nullptr;
        const bool hasR = ir >= 0 && ir < numInputs && inputs[ir] != nullptr;

        if (hasL)
            juce::FloatVectorOperations::copy (l, inputs[il] + offset, n);
        else
            juce::FloatVectorOperations::clear (l, n);

        if (hasR)
            juce::FloatVectorOperations::copy (r, inputs[ir] + offset, n);
        else if (hasL)
            juce::FloatVectorOperations::copy (r, l, n);
        else
            juce::FloatVectorOperations::clear (r, n);
    }

    renderClips (n, clock);      // audio clips from the timeline are added to the source

    if (isMono)
    {
        // one channel: (L + R) / 2; with a single input (or a mono source) both sides are equal, so nothing changes
        juce::FloatVectorOperations::add (l, r, n);
        juce::FloatVectorOperations::multiply (l, 0.5f, n);
        juce::FloatVectorOperations::copy (r, l, n);
    }

    renderClicks (n, clock);
    runChain (n, isMono);
    if (isMono)
        juce::FloatVectorOperations::copy (r, l, n);
    applyExtraDelay (n);

    // mono <-> stereo change: fade out before the plugins are re-configured, fade in afterwards
    {
        const float target = switching.load() ? 0.0f : 1.0f;
        if (fadeGain != 1.0f || target != 1.0f)
        {
            const float step = (target - fadeGain) / (float) juce::jmax (1, n);
            float f = fadeGain;
            for (int i = 0; i < n; ++i)
            {
                f += step;
                l[i] *= f;
                r[i] *= f;
            }
            fadeGain = target;
        }
    }

    // recording: everything on the track (input + clips, after the plugins), but not the metronome, which is added below
    if (recActive.load())
    {
        const juce::ScopedTryLock rl (recLock);
        if (rl.isLocked() && recWriter != nullptr)
        {
            const float* chans[2] = { l, r };
            recWriter->write (chans, n);   // a mono file takes only the first channel
        }
    }

    // the meter shows the level of the audio itself (after the plugins, before the metronome click, the fader and mute),
    // so neither the click nor the fader changes what the meter reads
    {
        float pl = 0.0f, pr = 0.0f;
        double sl = 0.0, sr2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            pl = juce::jmax (pl, std::abs (l[i]));
            pr = juce::jmax (pr, std::abs (r[i]));
            sl += (double) l[i] * l[i];
            sr2 += (double) r[i] * r[i];
        }
        peakL.store (juce::jmax (pl, peakL.load()));
        peakR.store (juce::jmax (pr, peakR.load()));
        const float rl = (float) std::sqrt (sl / (double) juce::jmax (1, n)), rr = (float) std::sqrt (sr2 / (double) juce::jmax (1, n));
        rmsL.store (juce::jmax (rl, rmsL.load()));
        rmsR.store (juce::jmax (rr, rmsR.load()));
    }

    juce::FloatVectorOperations::add (l, clickBuf.getReadPointer (0), n);
    juce::FloatVectorOperations::add (r, clickBuf.getReadPointer (1), n);

    // fader; the signal for other tracks (bus) is taken after the fader, before mute
    const float g = gainLinear.load();
    const int outCh = isMono ? outputChannel.load() : -1;
    const bool singleOut = outCh >= 0;
    const float p = juce::jlimit (-1.0f, 1.0f, pan.load());

    float targetL, targetR;
    if (isMono)
    {
        // a single output needs no pan; on a pair the constant-power law keeps the loudness while turning
        // (-3 dB on both sides in the middle, 0 dB at the edge)
        const float angle = (p + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        targetL = singleOut ? 1.0f : std::cos (angle);
        targetR = singleOut ? 1.0f : std::sin (angle);
    }
    else
    {
        targetL = p > 0.0f ? std::cos (p * juce::MathConstants<float>::halfPi) : 1.0f;
        targetR = p < 0.0f ? std::cos (-p * juce::MathConstants<float>::halfPi) : 1.0f;
    }

    const float stepL = (targetL - lastPanL) / (float) juce::jmax (1, n);
    const float stepR = (targetR - lastPanR) / (float) juce::jmax (1, n);
    float pL = lastPanL, pR = lastPanR;

    if (isMono)
    {
        // the bus gets the same signal on both sides (after the fader, before the pan)
        for (int i = 0; i < n; ++i)
            l[i] *= g;
        juce::FloatVectorOperations::copy (busOut.getWritePointer (0), l, n);
        juce::FloatVectorOperations::copy (busOut.getWritePointer (1), l, n);
        for (int i = 0; i < n; ++i)
        {
            pL += stepL;
            pR += stepR;
            const float v = l[i];
            l[i] = v * pL;
            r[i] = v * pR;
        }
    }
    else
    {
        for (int i = 0; i < n; ++i)
        {
            pL += stepL;
            pR += stepR;
            l[i] *= g * pL;
            r[i] *= g * pR;
        }
        juce::FloatVectorOperations::copy (busOut.getWritePointer (0), l, n);
        juce::FloatVectorOperations::copy (busOut.getWritePointer (1), r, n);
    }
    lastPanL = targetL;
    lastPanR = targetR;

    if (muted.load() || (othersSoloed.load() && ! soloed.load()))
    {
        juce::FloatVectorOperations::clear (l, n);
        juce::FloatVectorOperations::clear (r, n);
    }

    if (singleOut)
    {
        if (outCh < numOutputs && outputs[outCh] != nullptr)
            juce::FloatVectorOperations::add (outputs[outCh] + offset, l, n);
        return;
    }

    const int pair = outputPair.load();
    if (pair < 0)
        return;

    const int outL = pair * 2;
    const int outR = pair * 2 + 1;
    if (outL < numOutputs && outputs[outL] != nullptr)
        juce::FloatVectorOperations::add (outputs[outL] + offset, l, n);
    if (outR < numOutputs && outputs[outR] != nullptr)
        juce::FloatVectorOperations::add (outputs[outR] + offset, r, n);
}

void Track::runChain (int n, bool isMono)
{
    const juce::ScopedTryLock lock (chainLock);
    if (! lock.isLocked())
        return;

    for (auto& s : slots)
    {
        if (s == nullptr || s->plugin == nullptr || s->bypass.load())
            continue;

        auto* p = s->plugin.get();
        const int pluginChannels = juce::jmax (1, juce::jmax (p->getTotalNumInputChannels(), p->getTotalNumOutputChannels()));

        if (isMono || pluginChannels < 2)
        {
            // One channel goes through (a mono plugin works on one channel only: less load). A plugin that cannot do mono
            // gets the signal on both channels and we keep the left one.
            const int channels = juce::jlimit (1, pluginBuf.getNumChannels(), pluginChannels);
            for (int c = 0; c < channels; ++c)
            {
                auto* dest = pluginBuf.getWritePointer (c);
                if (c < 2)
                    juce::FloatVectorOperations::copy (dest, stereoBuf.getReadPointer (0), n);
                else
                    juce::FloatVectorOperations::clear (dest, n);
            }

            juce::AudioBuffer<float> view (pluginBuf.getArrayOfWritePointers(), channels, n);
            midi.clear();
            p->processBlock (view, midi);

            juce::FloatVectorOperations::copy (stereoBuf.getWritePointer (0), pluginBuf.getReadPointer (0), n);
            if (! isMono)   // a mono plugin on a stereo track (only until the layouts are conformed): same signal on both sides
                juce::FloatVectorOperations::copy (stereoBuf.getWritePointer (1), pluginBuf.getReadPointer (0), n);
            continue;
        }

        const int channels = juce::jlimit (2, pluginBuf.getNumChannels(), pluginChannels);
        for (int c = 0; c < channels; ++c)
        {
            auto* dest = pluginBuf.getWritePointer (c);
            if (c < 2)
                juce::FloatVectorOperations::copy (dest, stereoBuf.getReadPointer (c), n);
            else
                juce::FloatVectorOperations::clear (dest, n);
        }

        juce::AudioBuffer<float> view (pluginBuf.getArrayOfWritePointers(), channels, n);
        midi.clear();
        p->processBlock (view, midi);

        const int outs = p->getTotalNumOutputChannels();
        juce::FloatVectorOperations::copy (stereoBuf.getWritePointer (0), pluginBuf.getReadPointer (0), n);
        juce::FloatVectorOperations::copy (stereoBuf.getWritePointer (1),
                                           pluginBuf.getReadPointer (outs >= 2 ? 1 : 0), n);
    }
}

void Track::applyExtraDelay (int n)
{
    const int d = extraDelay.load();
    const int size = delayBuf.getNumSamples();
    if (d <= 0 || d >= size - maxBlock)
    {
        // keep the ring buffer filled so that switching compensation on does not play old audio
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* dst = delayBuf.getWritePointer (ch);
            auto* src = stereoBuf.getReadPointer (ch);
            for (int i = 0; i < n; ++i)
                dst[(delayWritePos + i) % size] = src[i];
        }
        delayWritePos = (delayWritePos + n) % size;
        return;
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        auto* ring = delayBuf.getWritePointer (ch);
        auto* io = stereoBuf.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
        {
            const int w = (delayWritePos + i) % size;
            ring[w] = io[i];
            io[i] = ring[(w - d + size) % size];
        }
    }
    delayWritePos = (delayWritePos + n) % size;
}

bool Track::startRecording (const juce::File& file, double sr, juce::TimeSliceThread& thread)
{
    stopRecording();
    file.getParentDirectory().createDirectory();
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;

    juce::WavAudioFormat wav;
    recChannels = mono.load() ? 1 : 2;   // a mono track is written as a mono file
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sr, (unsigned) recChannels, 24, {}, 0));
    if (writer == nullptr)
        return false;
    stream.release();   // the writer owns it now

    {
        const juce::ScopedLock sl (recLock);
        recWriter.reset (new juce::AudioFormatWriter::ThreadedWriter (writer.release(), thread, 65536));
    }
    recActive.store (true);
    return true;
}

void Track::stopRecording()
{
    recActive.store (false);
    const juce::ScopedLock sl (recLock);
    recWriter.reset();   // flushes and closes the file
}
