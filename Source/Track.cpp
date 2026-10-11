#include "Track.h"
#include "Text.h"

Track::Track (juce::String trackName, juce::AudioFormatManager& formats, juce::TimeSliceThread& thread)
    : name (std::move (trackName)), formatManager (formats), readThread (thread)
{
}

Track::~Track()
{
    transport.setSource (nullptr);
}

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

void Track::setPlayerSlot (int index)
{
    if (! juce::isPositiveAndBelow (index, numSlots) || findPlayerSlot() >= 0)
        return;

    auto slot = std::make_unique<Slot>();
    slot->builtinPlayer = true;
    slot->description.name = "TC Плеер"_ru;

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

    if (old->builtinPlayer)
    {
        stopPlayer();
    }
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

int Track::findPlayerSlot() const
{
    for (int i = 0; i < numSlots; ++i)
        if (slots[(size_t) i] != nullptr && slots[(size_t) i]->builtinPlayer)
            return i;
    return -1;
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

void Track::refreshLatency()
{
    cachedLatency.store (computeLatencySamples());

    bool active = false;
    for (auto& s : slots)
        if (s != nullptr && s->builtinPlayer && ! s->bypass.load())
            active = true;

    playerActive.store (active);
}

// ---------------------------------------------------------------- player

bool Track::loadFile (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
        return false;

    stopPlayer();
    hasFile = false;
    transport.setSource (nullptr);
    readerSource.reset();

    fileChannels = (int) reader->numChannels;
    const double fileRate = reader->sampleRate;

    readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
    readerSource->setLooping (looping);
    transport.setSource (readerSource.get(), 32768, &readThread, fileRate);
    transport.start();   // always "started": playback is gated by the track's own clock
    currentFile = file;
    hasFile = true;
    transport.setPosition (0.0);
    return true;
}

void Track::setClipOffset (double seconds)
{
    // the audio may start earlier than the grid only by the 4 count-in beats
    clipOffset.store (juce::jmax (seconds, -4.0 * beatSeconds()));
}

void Track::armPlayback (double startClockSeconds, double filePositionSeconds)
{
    if (! hasFile.load())
        return;

    playerState.store (0);
    transport.setPosition (juce::jmax (0.0, filePositionSeconds));
    pullStartClock.store (startClockSeconds);
    endedNaturally.store (false);
    playerState.store (1);
}

void Track::stopPlayer()
{
    playerState.store (0);
}

void Track::setLooping (bool shouldLoop)
{
    looping = shouldLoop;
    if (readerSource != nullptr)
        readerSource->setLooping (shouldLoop);
}

// ---------------------------------------------------------------- audio

void Track::prepare (double newSampleRate, int maxBlockSize, juce::AudioPlayHead* playHead)
{
    sampleRate = newSampleRate;
    maxBlock = juce::jmax (64, maxBlockSize);

    stereoBuf.setSize (2, maxBlock, false, true, false);
    busOut.setSize (2, maxBlock, false, true, false);
    clickBuf.setSize (2, maxBlock, false, true, false);
    pluginBuf.setSize (16, maxBlock, false, true, false);
    midi.ensureSize (2048);

    const int maxDelay = (int) (sampleRate * 2.0) + maxBlock;
    delayBuf.setSize (2, maxDelay, false, true, false);
    delayWritePos = 0;

    transport.prepareToPlay (maxBlock, sampleRate);

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
    transport.releaseResources();
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

// Renders this track's metronome clicks (into clickBuf) from the engine's global clock events
// and, when the built-in player is armed/playing, the audio file (into stereoBuf).
void Track::renderTransport (int n, const ChunkClock& clock)
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

    const int state = playerState.load();
    if (! clock.running || state == 0 || ! playerActive.load() || ! hasFile.load())
        return;

    int from = 0;
    if (state == 1)
    {
        const double startAt = pullStartClock.load();
        const double chunkEnd = clock.startSeconds + (double) n / sampleRate;
        if (startAt >= chunkEnd)
            return;   // not yet: wait for the clock to reach the start point

        from = juce::jlimit (0, n - 1, (int) std::lround ((startAt - clock.startSeconds) * sampleRate));
        playerState.store (2);
    }

    auto* l = stereoBuf.getWritePointer (0);
    auto* r = stereoBuf.getWritePointer (1);
    const int len = n - from;
    juce::AudioSourceChannelInfo info (&stereoBuf, from, len);
    transport.getNextAudioBlock (info);
    if (fileChannels == 1)
        juce::FloatVectorOperations::copy (r + from, l + from, len);

    if (transport.hasStreamFinished())
    {
        playerState.store (0);
        endedNaturally.store (true);
    }
}

void Track::processChunk (const float* const* inputs, int numInputs,
                          float* const* outputs, int numOutputs, int offset, int n,
                          const Track* busSource, const ChunkClock& clock)
{
    auto* l = stereoBuf.getWritePointer (0);
    auto* r = stereoBuf.getWritePointer (1);

    const bool player = playerActive.load();
    if (player)
    {
        stereoBuf.clear (0, n);
    }
    else if (busSource != nullptr && busSource != this && busSource->busOut.getNumSamples() >= n)
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

    renderTransport (n, clock);
    runChain (n);
    applyExtraDelay (n);

    juce::FloatVectorOperations::add (l, clickBuf.getReadPointer (0), n);
    juce::FloatVectorOperations::add (r, clickBuf.getReadPointer (1), n);

    // fader; the signal for other tracks (bus) is taken after the fader, before mute
    const float g = gainLinear.load();
    for (int i = 0; i < n; ++i)
    {
        l[i] *= g;
        r[i] *= g;
    }
    juce::FloatVectorOperations::copy (busOut.getWritePointer (0), l, n);
    juce::FloatVectorOperations::copy (busOut.getWritePointer (1), r, n);

    if (muted.load())
    {
        juce::FloatVectorOperations::clear (l, n);
        juce::FloatVectorOperations::clear (r, n);
    }

    float pl = 0.0f, pr = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        pl = juce::jmax (pl, std::abs (l[i]));
        pr = juce::jmax (pr, std::abs (r[i]));
    }
    peakL.store (juce::jmax (pl, peakL.load()));
    peakR.store (juce::jmax (pr, peakR.load()));

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

void Track::runChain (int n)
{
    const juce::ScopedTryLock lock (chainLock);
    if (! lock.isLocked())
        return;

    for (auto& s : slots)
    {
        if (s == nullptr || s->plugin == nullptr || s->bypass.load())
            continue;

        auto* p = s->plugin.get();
        const int channels = juce::jlimit (2, pluginBuf.getNumChannels(),
                                           juce::jmax (p->getTotalNumInputChannels(),
                                                       p->getTotalNumOutputChannels()));

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
