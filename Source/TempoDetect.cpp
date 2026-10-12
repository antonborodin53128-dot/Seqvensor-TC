#include "TempoDetect.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

// Tempo estimate: multi-band spectral-flux onset envelope -> enhanced autocorrelation with a tempo prior
// -> fine comb fit over the whole file (0.01 BPM steps). Also returns a confidence so weak results
// (vocals, pads, noise) can be rejected or replaced by the tempo of a stronger stem.
// Self-contained (own FFT) so it can be tested outside of JUCE.

namespace
{
    void fftInPlace (std::vector<std::complex<float>>& a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap (a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const float ang = -6.28318530718f / (float) len;
            const std::complex<float> wl (std::cos (ang), std::sin (ang));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<float> w (1.0f, 0.0f);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }
}

TempoResult detectTempoEx (const std::vector<float>& monoIn, double sr)
{
    TempoResult out;
    if (sr <= 0.0 || (double) monoIn.size() < sr * 6.0)
        return out;

    // analyse at most 3 minutes, taken from the middle (intros are often not representative)
    const size_t maxLen = (size_t) (sr * 180.0);
    size_t offset = 0, total = monoIn.size();
    if (total > maxLen)
    {
        offset = (total - maxLen) / 2;
        total = maxLen;
    }
    const float* mono = monoIn.data() + offset;

    const int fftSize = sr > 70000.0 ? 4096 : 2048;
    const int hop = std::max (64, (int) std::lround (sr / 200.0));   // ~5 ms
    const int bins = fftSize / 2;
    const int frames = (int) (((int64_t) total - fftSize) / hop);
    if (frames < 400)
        return out;

    // band edges (Hz): bass drum / bass, low mids, mids, presence, hats
    const double edges[] = { 35.0, 140.0, 400.0, 1200.0, 3600.0, 10000.0 };
    constexpr int numBands = 5;
    int bandOf[8192] = {};
    for (int k = 0; k < bins; ++k)
    {
        const double f = k * sr / fftSize;
        int b = -1;
        for (int i = 0; i < numBands; ++i)
            if (f >= edges[i] && f < edges[i + 1])
                b = i;
        bandOf[k] = b;
    }

    std::vector<float> window ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (6.28318530718f * (float) i / (float) (fftSize - 1));

    std::vector<std::complex<float>> buf ((size_t) fftSize);
    std::vector<float> prev ((size_t) bins, 0.0f);
    std::vector<std::vector<float>> band ((size_t) numBands, std::vector<float> ((size_t) frames, 0.0f));

    for (int f = 0; f < frames; ++f)
    {
        const float* src = mono + (size_t) f * hop;
        for (int i = 0; i < fftSize; ++i)
            buf[(size_t) i] = std::complex<float> (src[i] * window[(size_t) i], 0.0f);
        fftInPlace (buf);

        for (int k = 1; k < bins; ++k)
        {
            const int b = bandOf[k];
            const float m = std::log (1.0f + 200.0f * std::abs (buf[(size_t) k]) / (float) fftSize * 4.0f);
            if (b >= 0)
                band[(size_t) b][(size_t) f] += std::max (0.0f, m - prev[(size_t) k]);
            prev[(size_t) k] = m;
        }
    }

    // balance the bands so a quiet but regular band (hats, bass) still counts
    std::vector<double> means ((size_t) numBands, 0.0);
    double meanOfMeans = 0.0;
    for (int b = 0; b < numBands; ++b)
    {
        double s = 0.0;
        for (float v : band[(size_t) b])
            s += v;
        means[(size_t) b] = s / frames;
        meanOfMeans += means[(size_t) b] / numBands;
    }
    if (meanOfMeans < 0.05)
        return out;   // silence

    std::vector<float> env ((size_t) frames, 0.0f);
    for (int b = 0; b < numBands; ++b)
    {
        const float norm = (float) (1.0 / (means[(size_t) b] + 0.3 * meanOfMeans + 0.5));
        for (int f = 0; f < frames; ++f)
            env[(size_t) f] += band[(size_t) b][(size_t) f] * norm;
    }

    // remove the slow trend, keep only the peaks above the local average
    const double fps = sr / hop;
    const int w = std::max (3, (int) (fps * 0.35));
    std::vector<float> flat ((size_t) frames, 0.0f);
    double run = 0.0;
    for (int i = 0; i < frames; ++i)
    {
        run += env[(size_t) i];
        if (i >= 2 * w + 1)
            run -= env[(size_t) (i - 2 * w - 1)];
        const int cnt = std::min (i + 1, 2 * w + 1);
        flat[(size_t) i] = std::max (0.0f, env[(size_t) i] - (float) (run / cnt));
    }

    const int lagMin = (int) std::floor (fps * 60.0 / 210.0);
    const int lagMax = (int) std::ceil (fps * 60.0 / 55.0);
    const bool useQuad = lagMax * 4 + 2 < frames / 2;
    const int lagLimit = useQuad ? lagMax * 4 + 2 : lagMax * 2 + 2;
    if (lagLimit >= frames / 2)
        return out;

    std::vector<double> ac ((size_t) lagLimit + 2, 0.0);
    for (int lag = lagMin - 1; lag <= lagLimit; ++lag)
    {
        double sum = 0.0;
        for (int i = 0; i + lag < frames; ++i)
            sum += (double) flat[(size_t) i] * flat[(size_t) (i + lag)];
        ac[(size_t) lag] = sum / (double) (frames - lag);
    }

    // enhanced score: a real beat also repeats at 2x and 4x its period
    std::vector<double> score ((size_t) lagMax + 2, 0.0);
    double sumScore = 0.0;
    int count = 0;
    int bestLag = 0;
    double best = 0.0;
    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        const double bpm = 60.0 * fps / lag;
        const double prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 115.0) / 0.85, 2.0));
        double s = ac[(size_t) lag] + 0.6 * ac[(size_t) lag * 2] + (useQuad ? 0.3 * ac[(size_t) lag * 4] : 0.0);
        // a tempo that is really twice as fast would show up half-way as well: reward that
        if (lag % 2 == 0)
            s += 0.1 * ac[(size_t) lag / 2];
        s *= prior;
        score[(size_t) lag] = s;
        sumScore += s;
        ++count;
        if (s > best)
        {
            best = s;
            bestLag = lag;
        }
    }
    if (bestLag <= 0 || best <= 0.0 || count == 0)
        return out;

    // confidence: how far the winner stands out from the average candidate, and how periodic the signal is
    const double prominence = best / (sumScore / count);             // 1 = no structure
    double energy = 0.0;
    for (float v : flat)
        energy += (double) v * v;
    energy /= frames;
    const double regularity = ac[(size_t) bestLag] / std::max (1.0e-12, energy);   // how much of the envelope repeats
    const double conf = std::max (0.0, std::min (1.0, (prominence - 1.6) / 3.0)) * std::max (0.0, std::min (1.0, regularity / 0.12));

    // parabolic refinement
    double lagF = bestLag;
    const double y0 = ac[(size_t) bestLag - 1], y1 = ac[(size_t) bestLag], y2 = ac[(size_t) bestLag + 1];
    const double den = y0 - 2.0 * y1 + y2;
    if (std::abs (den) > 1.0e-12)
        lagF += std::max (-1.0, std::min (1.0, 0.5 * (y0 - y2) / den));
    double bpm = 60.0 * fps / lagF;

    // Fine refinement: slide a beat comb (period + phase) over the whole envelope in 0.01 BPM steps,
    // since the coarse estimate is only good to ~0.5 BPM, which drifts by whole beats over a song.
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

    out.bpm = std::round (bpm * 100.0) / 100.0;
    out.confidence = conf;
    return out;
}

double detectTempo (const std::vector<float>& mono, double sr)
{
    const auto r = detectTempoEx (mono, sr);
    return r.confidence >= tempoMinConfidence ? r.bpm : 0.0;
}
