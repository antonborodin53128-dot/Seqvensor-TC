#include "Track.h"

Track::Track (Kind kindToUse, juce::String trackName,
              juce::AudioFormatManager& formats, juce::TimeSliceThread& thread)
    : kind (kindToUse), name (std::move (trackName)), formatManager (formats), readThread (thread)
{
    if (kind == Kind::Player)
    {
        inputL = -1;
        inputR = -1;
    }
}

Track::~Track()
{
    transport.setSource (nullptr);
}

// ---------------------------------------------------------------- chain

void Track::addPlugin (std::unique_ptr<juce::AudioPluginInstance> plugin,
                       const juce::PluginDescription& description)
{
    if (plugin == nullptr)
        return;

    auto slot = std::make_unique<Slot>();
    slot->description = description;
    slot->plugin = std::move (plugin);

    {
        const juce::ScopedLock sl (chainLock);
        slots.push_back (std::move (slot));
    }
    refreshLatency();
}

void Track::removePlugin (int index)
{
    std::unique_ptr<Slot> removed;
    {
        const juce::ScopedLock sl (chainLock);
        if (! juce::isPositiveAndBelow (index, (int) slots.size()))
            return;
        removed = std::move (slots[(size_t) index]);
        slots.erase (slots.begin() + index);
    }

    if (removed != nullptr && removed->plugin != nullptr)
    {
        removed->plugin->releaseResources();
        removed->plugin->setPlayHead (nullptr);
    }
    refreshLatency();
}

int Track::getNumPlugins() const
{
    return (int) slots.size();
}

Track::Slot* Track::getSlot (int index) const
{
    return juce::isPositiveAndBelow (index, (int) slots.size()) ? slots[(size_t) index].get() : nullptr;
}

int Track::computeLatencySamples() const
{
    int total = 0;
    for (auto& s : slots)
        if (s->plugin != nullptr && ! s->bypass.load())
            total += s->plugin->getLatencySamples();
    return total;
}

// ---------------------------------------------------------------- player

bool Track::loadFile (const juce::File& file)
{
    if (kind != Kind::Player)
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
        return false;

    transport.stop();
    transport.setSource (nullptr);
    readerSource.reset();

    fileChannels = (int) reader->numChannels;
    const double fileRate = reader->sampleRate;

    readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
    readerSource->setLooping (looping);
    transport.setSource (readerSource.get(), 32768, &readThread, fileRate);
    currentFile = file;
    return true;
}

void Track::play()
{
    if (readerSource != nullptr)
        transport.start();
}

void Track::pause()
{
    transport.stop();
}

void Track::stopAndRewind()
{
    transport.stop();
    transport.setPosition (0.0);
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
    pluginBuf.setSize (16, maxBlock, false, true, false);
    midi.ensureSize (2048);

    const int maxDelay = (int) (sampleRate * 2.0) + maxBlock;
    delayBuf.setSize (2, maxDelay, false, true, false);
    delayWritePos = 0;

    transport.prepareToPlay (maxBlock, sampleRate);

    const juce::ScopedLock sl (chainLock);
    for (auto& s : slots)
    {
        if (s->plugin == nullptr)
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
        if (s->plugin != nullptr)
            s->plugin->releaseResources();
}

void Track::process (const float* const* inputs, int numInputs,
                     float* const* outputs, int numOutputs, int numSamples)
{
    int done = 0;
    while (done < numSamples)
    {
        const int n = juce::jmin (maxBlock, numSamples - done);
        processChunk (inputs, numInputs, outputs, numOutputs, done, n);
        done += n;
    }
}

void Track::processChunk (const float* const* inputs, int numInputs,
                          float* const* outputs, int numOutputs, int offset, int n)
{
    auto* l = stereoBuf.getWritePointer (0);
    auto* r = stereoBuf.getWritePointer (1);

    if (kind == Kind::Live)
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
    else
    {
        stereoBuf.clear (0, n);
        if (readerSource != nullptr)
        {
            juce::AudioSourceChannelInfo info (&stereoBuf, 0, n);
            transport.getNextAudioBlock (info);
            if (fileChannels == 1)
                juce::FloatVectorOperations::copy (r, l, n);
        }
    }

    runChain (n);
    applyExtraDelay (n);

    // gain, mute, meters, sum into the chosen output pair
    const float g = muted.load() ? 0.0f : gainLinear.load();
    float pl = 0.0f, pr = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        l[i] *= g;
        r[i] *= g;
        pl = juce::jmax (pl, std::abs (l[i]));
        pr = juce::jmax (pr, std::abs (r[i]));
    }
    peakL.store (pl);
    peakR.store (pr);

    const int pair = outputPair.load();
    const int outL = pair * 2;
    const int outR = pair * 2 + 1;
    if (outL >= 0 && outL < numOutputs && outputs[outL] != nullptr)
        juce::FloatVectorOperations::add (outputs[outL] + offset, l, n);
    if (outR >= 0 && outR < numOutputs && outputs[outR] != nullptr)
        juce::FloatVectorOperations::add (outputs[outR] + offset, r, n);
}

void Track::runChain (int n)
{
    const juce::ScopedTryLock lock (chainLock);
    if (! lock.isLocked())
        return;

    for (auto& s : slots)
    {
        if (s->plugin == nullptr || s->bypass.load())
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
