#include "TempoDetect.h"

#include <juce_dsp/juce_dsp.h>

// Tempo estimate: spectral-flux onset envelope + autocorrelation. Returns 0 if nothing sensible was found.
double detectTempo (const std::vector<float>& mono, double sr)
{
    const juce::int64 total = (juce::int64) mono.size();
    if (sr <= 0.0 || total < (juce::int64) (sr * 8.0))
        return 0.0;

    constexpr int order = 10, fftSize = 1 << order, hop = 256, bins = fftSize / 2;
    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann);

    const int frames = (int) ((total - fftSize) / hop);
    if (frames < 200)
        return 0.0;

    std::vector<float> env ((size_t) frames, 0.0f), prev ((size_t) bins, 0.0f), data ((size_t) fftSize * 2);
    for (int f = 0; f < frames; ++f)
    {
        std::fill (data.begin(), data.end(), 0.0f);
        std::copy (mono.begin() + (size_t) f * hop, mono.begin() + (size_t) f * hop + fftSize, data.begin());
        window.multiplyWithWindowingTable (data.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (data.data());

        float flux = 0.0f;
        for (int k = 2; k < bins / 2; ++k)
        {
            const float m = std::log (1.0f + 100.0f * data[(size_t) k]);
            flux += juce::jmax (0.0f, m - prev[(size_t) k]);
            prev[(size_t) k] = m;
        }
        env[(size_t) f] = flux;
    }

    // remove the slow trend
    const double fps = sr / hop;
    const int w = juce::jmax (3, (int) (fps * 0.4));
    std::vector<float> flat ((size_t) frames, 0.0f);
    double run = 0.0;
    for (int i = 0; i < frames; ++i)
    {
        run += env[(size_t) i];
        if (i >= 2 * w + 1)
            run -= env[(size_t) (i - 2 * w - 1)];
        const int cnt = juce::jmin (i + 1, 2 * w + 1);
        const double mean = run / cnt;
        flat[(size_t) i] = juce::jmax (0.0f, env[(size_t) i] - (float) mean);
    }

    const int lagMin = (int) std::floor (fps * 60.0 / 200.0);
    const int lagMax = (int) std::ceil (fps * 60.0 / 60.0);
    const int lagLimit = lagMax * 2 + 2;
    if (lagLimit >= frames / 2)
        return 0.0;

    std::vector<double> ac ((size_t) lagLimit + 2, 0.0);
    for (int lag = lagMin - 1; lag <= lagLimit; ++lag)
    {
        double sum = 0.0;
        for (int i = 0; i + lag < frames; ++i)
            sum += (double) flat[(size_t) i] * flat[(size_t) (i + lag)];
        ac[(size_t) lag] = sum / (double) (frames - lag);
    }

    int bestLag = 0;
    double best = 0.0;
    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        const double bpm = 60.0 * fps / lag;
        const double prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 120.0) / 0.9, 2.0));
        const double score = prior * (ac[(size_t) lag] + 0.5 * ac[(size_t) (lag * 2)]);
        if (score > best)
        {
            best = score;
            bestLag = lag;
        }
    }
    if (bestLag <= 0)
        return 0.0;

    // parabolic refinement
    double lagF = bestLag;
    const double y0 = ac[(size_t) bestLag - 1], y1 = ac[(size_t) bestLag], y2 = ac[(size_t) bestLag + 1];
    const double den = y0 - 2.0 * y1 + y2;
    if (std::abs (den) > 1.0e-12)
        lagF += juce::jlimit (-1.0, 1.0, 0.5 * (y0 - y2) / den);

    double bpm = 60.0 * fps / lagF;

    // Fine refinement: the coarse estimate is only accurate to ~0.5 BPM, which drifts by whole beats over a song.
    // Slide a beat comb (period + phase) over the whole onset envelope and keep the best fit at 0.01 BPM steps.
    {
        auto envAt = [&] (double pos)
        {
            if (pos < 0.0 || pos >= frames - 1)
                return 0.0f;
            const int i = (int) pos;
            const float fr = (float) (pos - i);
            return flat[(size_t) i] * (1.0f - fr) + flat[(size_t) i + 1] * fr;
        };

        double bestScore = -1.0, bestBpm = bpm;
        for (double cand = bpm * 0.97; cand <= bpm * 1.03; cand += 0.01)
        {
            const double period = fps * 60.0 / cand;
            for (int ph = 0; ph < 24; ++ph)
            {
                double sc = 0.0;
                for (double pos = period * ph / 24.0; pos < frames - 1; pos += period)
                    sc += envAt (pos);
                if (sc > bestScore)
                {
                    bestScore = sc;
                    bestBpm = cand;
                }
            }
        }
        bpm = bestBpm;
    }

    while (bpm < 70.0) bpm *= 2.0;
    while (bpm > 180.0) bpm *= 0.5;
    return std::round (bpm * 100.0) / 100.0;
}
