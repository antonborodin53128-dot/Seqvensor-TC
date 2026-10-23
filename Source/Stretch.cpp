#include "Stretch.h"

#include "signalsmith-stretch.h"

#include <cmath>
#include <new>

std::shared_ptr<ClipData> stretchClipData (const ClipData& source, double timeRatio,
                                           const std::function<bool()>& cancelled)
{
    const int channels = juce::jlimit (1, 2, source.audio.getNumChannels());
    const int inLength = source.numSamples;
    const int outLength = juce::jmax (1, (int) std::llround ((double) inLength * timeRatio));
    if (inLength <= 0 || timeRatio <= 0.0)
        return nullptr;

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault (channels, (float) source.sampleRate);
    const int inLatency = stretch.inputLatency();
    const int outLatency = stretch.outputLatency();

    auto result = std::make_shared<ClipData>();
    result->name = source.name;
    result->file = source.file;
    result->fileBpm = source.fileBpm;
    result->sampleRate = source.sampleRate;
    result->numSamples = outLength;
    try
    {
        result->audio.setSize (channels, outLength);
        result->audio.clear();
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;   // not enough memory for the stretched copy: the clip keeps playing unstretched
    }

    // the processing time starts at the first sample: hand over the first inLatency samples as pre-roll
    std::vector<std::vector<float>> lead ((size_t) channels, std::vector<float> ((size_t) inLatency, 0.0f));
    std::vector<const float*> leadPtr ((size_t) channels);
    for (int c = 0; c < channels; ++c)
    {
        const int n = juce::jmin (inLatency, inLength);
        std::copy (source.audio.getReadPointer (c), source.audio.getReadPointer (c) + n, lead[(size_t) c].begin());
        leadPtr[(size_t) c] = lead[(size_t) c].data();
    }
    stretch.seek (leadPtr, inLatency, 1.0 / timeRatio);

    constexpr int block = 4096;
    std::vector<std::vector<float>> blockIn ((size_t) channels, std::vector<float> (block));
    std::vector<std::vector<float>> blockOut ((size_t) channels);
    std::vector<const float*> inPtr ((size_t) channels);
    std::vector<float*> outPtr ((size_t) channels);

    long long written = 0;   // samples of the stream so far; the first outLatency of them are pre-roll
    auto store = [&] (int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const long long dst = written + i - outLatency;
            if (dst >= 0 && dst < outLength)
                for (int c = 0; c < channels; ++c)
                    result->audio.setSample (c, (int) dst, blockOut[(size_t) c][(size_t) i]);
        }
        written += n;
    };

    // input positions [inLatency, inLength + inLatency): the real audio followed by inLatency samples of silence
    const long long total = inLength;
    for (long long pos = 0; pos < total; pos += block)
    {
        if (cancelled && cancelled())
            return nullptr;

        const int nIn = (int) juce::jmin<long long> (block, total - pos);
        for (int c = 0; c < channels; ++c)
        {
            const float* src = source.audio.getReadPointer (c);
            for (int i = 0; i < nIn; ++i)
            {
                const long long s = (long long) inLatency + pos + i;
                blockIn[(size_t) c][(size_t) i] = s < inLength ? src[s] : 0.0f;
            }
            inPtr[(size_t) c] = blockIn[(size_t) c].data();
        }

        const long long o0 = std::llround ((double) pos * timeRatio);
        const long long o1 = std::llround ((double) (pos + nIn) * timeRatio);
        const int nOut = (int) (o1 - o0);
        for (int c = 0; c < channels; ++c)
        {
            blockOut[(size_t) c].assign ((size_t) juce::jmax (1, nOut), 0.0f);
            outPtr[(size_t) c] = blockOut[(size_t) c].data();
        }
        stretch.process (inPtr, nIn, outPtr, nOut);
        store (nOut);
    }

    // the tail still inside the stretcher
    for (int c = 0; c < channels; ++c)
    {
        blockOut[(size_t) c].assign ((size_t) juce::jmax (1, outLatency), 0.0f);
        outPtr[(size_t) c] = blockOut[(size_t) c].data();
    }
    stretch.flush (outPtr, outLatency);
    store (outLatency);

    result->buildPeaks();
    return result;
}
