#include "LiveStretch.h"

#include "signalsmith-stretch.h"

#include <algorithm>
#include <cmath>

struct LiveStretch::Impl
{
    std::shared_ptr<const ClipData> keep;
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    juce::AudioBuffer<float> resampled;
    const float* data[2] = { nullptr, nullptr };
    int channels = 1;
    long long numSamples = 0;
    int inLatency = 0, outLatency = 0;
    long long feedPos = 0;   // next input sample (at the device rate) the stretcher will get

    static constexpr int piece = 1024;
    int inCap = 0, outCap = 0;
    std::vector<std::vector<float>> inBuf, outBuf;
    std::vector<const float*> inPtr;
    std::vector<float*> outPtr;

    float sampleAt (int ch, long long i, bool loop) const
    {
        if (numSamples <= 0)
            return 0.0f;
        if (loop)
        {
            i %= numSamples;
            if (i < 0)
                i += numSamples;
        }
        else if (i < 0 || i >= numSamples)
            return 0.0f;
        return data[std::min (ch, channels - 1)][i];
    }

    void fill (long long from, int count, bool loop)
    {
        for (int c = 0; c < channels; ++c)
        {
            float* dst = inBuf[(size_t) c].data();
            if (! loop && from >= 0 && from + count <= numSamples)
            {
                const float* src = data[std::min (c, channels - 1)] + from;
                std::copy (src, src + count, dst);
            }
            else
            {
                for (int i = 0; i < count; ++i)
                    dst[i] = sampleAt (c, from + i, loop);
            }
            inPtr[(size_t) c] = dst;
        }
    }
};

LiveStretch::LiveStretch (std::shared_ptr<const ClipData> original, double deviceRate, bool buildSource)
    : impl (new Impl()), rate (deviceRate)
{
    auto& m = *impl;
    m.keep = original;
    m.channels = juce::jlimit (1, 2, original->audio.getNumChannels());
    m.stretch.presetDefault (m.channels, (float) deviceRate);
    m.inLatency = m.stretch.inputLatency();
    m.outLatency = m.stretch.outputLatency();

    const int cap = std::max (Impl::piece, m.outLatency);
    m.outCap = cap + 16;
    m.inCap = cap * 4 + 256;
    m.inBuf.assign ((size_t) m.channels, std::vector<float> ((size_t) std::max (m.inCap, m.inLatency + 16)));
    m.outBuf.assign ((size_t) m.channels, std::vector<float> ((size_t) m.outCap));
    m.inPtr.assign ((size_t) m.channels, nullptr);
    m.outPtr.assign ((size_t) m.channels, nullptr);

    if (std::abs (original->sampleRate - deviceRate) < 0.5)
    {
        for (int c = 0; c < m.channels; ++c)
            m.data[c] = original->audio.getReadPointer (c);
        m.numSamples = original->numSamples;
        ready.store (true);
    }
    else if (buildSource && original->numSamples > 0)
    {
        const double ratio = original->sampleRate / deviceRate;
        const int outLen = (int) std::ceil ((double) original->numSamples / ratio);
        m.resampled.setSize (m.channels, outLen + 8);
        m.resampled.clear();
        std::vector<float> padded ((size_t) original->numSamples + 128, 0.0f);
        for (int c = 0; c < m.channels; ++c)
        {
            std::copy (original->audio.getReadPointer (c), original->audio.getReadPointer (c) + original->numSamples, padded.begin());
            juce::WindowedSincInterpolator interp;
            interp.process (ratio, padded.data(), m.resampled.getWritePointer (c), outLen);
            m.data[c] = m.resampled.getReadPointer (c);
        }
        m.numSamples = outLen;
        ready.store (true);
    }
}

LiveStretch::~LiveStretch() = default;

void LiveStretch::reseek (double posStart, double rho, bool loop)
{
    auto& m = *impl;
    const long long p = std::llround (posStart);

    // the first inLatency samples after the position are the pre-roll the stretcher starts from
    m.fill (p, m.inLatency, loop);
    m.stretch.reset();
    m.stretch.seek (m.inPtr, m.inLatency, rho);
    m.feedPos = p + m.inLatency;

    // run the stretcher through its output latency: after that its output is aligned with the position
    const int primeIn = std::max (1, (int) std::llround ((double) m.outLatency * rho));
    const int primeOut = m.outLatency;
    if (primeOut > 0 && primeIn <= m.inCap && primeOut <= m.outCap)
    {
        m.fill (m.feedPos, primeIn, loop);
        for (int c = 0; c < m.channels; ++c)
            m.outPtr[(size_t) c] = m.outBuf[(size_t) c].data();
        m.stretch.process (m.inPtr, primeIn, m.outPtr, primeOut);
        m.feedPos += primeIn;
    }
    primed = true;
}

void LiveStretch::renderPiece (float* outL, float* outR, int n, double posStart, double rho, bool loop)
{
    auto& m = *impl;

    // the stretcher is ahead of its output by its latency: feed it that far in advance
    const double target = posStart + (double) m.inLatency + (double) m.outLatency * rho;
    if (! primed || std::abs ((double) m.feedPos - target) > 12000.0)
        reseek (posStart, rho, loop);

    // a gentle servo keeps the stretcher on the position given by the song clock (rounding, tempo changes)
    const double nominal = (double) n * rho;
    const double err = target - (double) m.feedPos;
    const double lim = 0.15 * nominal + 2.0;
    const double corr = std::max (-lim, std::min (lim, err * 0.2));
    const int nIn = std::max (1, std::min (m.inCap, (int) std::llround (nominal + corr)));

    m.fill (m.feedPos, nIn, loop);
    for (int c = 0; c < m.channels; ++c)
        m.outPtr[(size_t) c] = m.outBuf[(size_t) c].data();
    m.stretch.process (m.inPtr, nIn, m.outPtr, n);
    m.feedPos += nIn;

    const float* a = m.outBuf[0].data();
    const float* b = m.channels > 1 ? m.outBuf[1].data() : a;
    for (int i = 0; i < n; ++i)
    {
        outL[i] += a[i];
        outR[i] += b[i];
    }
}

void LiveStretch::render (float* outL, float* outR, int n, double posStart, double rho, bool loop)
{
    if (! ready.load())
        return;
    for (int done = 0; done < n; done += Impl::piece)
    {
        const int m = std::min (Impl::piece, n - done);
        renderPiece (outL + done, outR + done, m, posStart + (double) done * rho, rho, loop);
    }
}
