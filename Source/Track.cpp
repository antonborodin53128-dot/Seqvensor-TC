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
        haltTransport();
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

    haltTransport();
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
    transport.setPosition (startPositionSeconds());
    return true;
}

void Track::setClipOffset (double seconds)
{
    clipOffset.store (seconds);
    if (hasFile.load() && ! running.load())
        transport.setPosition (startPositionSeconds());
}

void Track::prepareStart()
{
    if (hasFile.load())
        transport.setPosition (startPositionSeconds());
}

void Track::play()
{
    if (! playerActive.load() || ! hasFile.load())
        return;
    prepareStart();
    startRequest.store (0);
}

void Track::haltTransport()
{
    startRequest.store (-1);
    running.store (false);
}

void Track::stopAndRewind()
{
    haltTransport();
    if (hasFile.load())
        transport.setPosition (startPositionSeconds());
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

// Runs the track's own clock: count-in, metronome clicks (into clickBuf) and, when the
// built-in player is active, the audio file (into stereoBuf), shifted by clipOffset.
void Track::renderTransport (int n)
{
    clickBuf.clear (0, n);

    const int req = startRequest.exchange (-1);
    if (req >= 0)
    {
        beatIndex = 0;
        nextBeatIn = 0.0;
        countIn = req;
        musicStarted = (req == 0);
        fileDelayLeft = juce::jmax (0, (int) std::lround (clipOffset.load() * sampleRate));
        clickPos = -1;
        running.store (true);
    }

    if (! running.load())
        return;

    auto* l = stereoBuf.getWritePointer (0);
    auto* r = stereoBuf.getWritePointer (1);
    auto* cl = clickBuf.getWritePointer (0);
    auto* cr = clickBuf.getWritePointer (1);

    const double bpm = juce::jlimit (20.0, 400.0, bpmSource != nullptr ? bpmSource->load() : 120.0);
    const double beatSamples = sampleRate * 60.0 / bpm;
    const bool metro = metronomeOn.load();
    const bool player = playerActive.load() && hasFile.load();

    int pos = 0;
    while (pos < n)
    {
        if (nextBeatIn <= 0.0)
        {
            if (! musicStarted && beatIndex == countIn)
                musicStarted = true;   // the count-in is over: the music starts on this beat

            if (metro)
            {
                clickPos = 0;
                clickAccent = (beatIndex % 4) == 0;
            }
            ++beatIndex;
            nextBeatIn += beatSamples;
        }

        const int seg = juce::jmin (n - pos, juce::jmax (1, (int) std::ceil (nextBeatIn)));

        if (musicStarted && player)
        {
            const int skip = juce::jmin (seg, fileDelayLeft);
            fileDelayLeft -= skip;
            const int len = seg - skip;
            if (len > 0)
            {
                juce::AudioSourceChannelInfo info (&stereoBuf, pos + skip, len);
                transport.getNextAudioBlock (info);
                if (fileChannels == 1)
                    juce::FloatVectorOperations::copy (r + pos + skip, l + pos + skip, len);
            }
        }

        renderClick (cl + pos, cr + pos, seg);

        nextBeatIn -= (double) seg;
        pos += seg;
    }

    if (musicStarted && player && fileDelayLeft == 0 && transport.hasStreamFinished())
        running.store (false);
}

void Track::processChunk (const float* const* inputs, int numInputs,
                          float* const* outputs, int numOutputs, int offset, int n,
                          const Track* busSource)
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

    renderTransport (n);
    runChain (n);
    applyExtraDelay (n);

    if (running.load() || startRequest.load() >= 0)
    {
        juce::FloatVectorOperations::add (l, clickBuf.getReadPointer (0), n);
        juce::FloatVectorOperations::add (r, clickBuf.getReadPointer (1), n);
    }

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
