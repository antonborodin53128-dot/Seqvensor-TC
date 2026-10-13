#include "LiveStretch.h"

#include "signalsmith-stretch.h"

#include <algorithm>
#include <array>
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

    // Pitch change by resampling: the stretcher only stretches time (as when following the tempo), its output
    // (the "mid" stream) is then read faster or slower by the pitch factor with a windowed-sinc resampler.
    // So the tempo stays, and the whole sound is shifted the way a normal key change does it.
    static constexpr int sincHalf = 16, sincPhases = 256;
    double pitch = 1.0;              // current factor (2^(semitones/12))
    double tableFor = -1.0;          // factor the table was built for
    std::vector<float> table;        // (sincPhases + 1) rows of 2 * sincHalf taps
    std::vector<std::vector<float>> mid;   // stretcher output not consumed yet, per channel
    long long midBase = 0;           // absolute index of mid[ch][0] in the stream since the last reseek
    long long midCount = 0;          // mid samples produced since the last reseek
    double midRead = 0.0;            // absolute position of the next final output sample in the mid stream

    void buildTable (double f)
    {
        tableFor = f;
        table.assign ((size_t) (sincPhases + 1) * 2 * sincHalf, 0.0f);
        const double pi = 3.14159265358979323846;
        const double cutoff = 0.97 * std::min (1.0, 1.0 / f);   // below the new Nyquist: no aliasing when shifting up
        for (int ph = 0; ph <= sincPhases; ++ph)
        {
            const double frac = (double) ph / sincPhases;
            double sum = 0.0;
            double* row = nullptr;
            std::array<double, 2 * sincHalf> tmp {};
            row = tmp.data();
            for (int t = 0; t < 2 * sincHalf; ++t)
            {
                const double x = (double) (t - sincHalf + 1) - frac;       // distance of the tap from the output position
                const double arg = pi * x * cutoff;
                const double sinc = std::abs (arg) < 1.0e-9 ? 1.0 : std::sin (arg) / arg;
                const double w = x / (double) sincHalf;                      // -1..1
                const double win = std::abs (w) >= 1.0 ? 0.0 : 0.42 + 0.5 * std::cos (pi * w) + 0.08 * std::cos (2.0 * pi * w);
                row[t] = sinc * win;
                sum += row[t];
            }
            for (int t = 0; t < 2 * sincHalf; ++t)
                table[(size_t) ph * 2 * sincHalf + (size_t) t] = (float) (row[t] / (sum != 0.0 ? sum : 1.0));
        }
    }

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
    m.outCap = std::max (cap, 2 * Impl::piece + 4 * Impl::sincHalf) + 64;
    m.inCap = cap * 4 + 2048;
    m.mid.assign ((size_t) m.channels, {});
    for (auto& v : m.mid)
        v.reserve ((size_t) 16384);
    m.table.reserve ((size_t) (Impl::sincPhases + 1) * 2 * Impl::sincHalf);
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
    const double rate = rho / m.pitch;   // input samples per sample of the stretcher's output

    // the first inLatency samples after the position are the pre-roll the stretcher starts from
    m.fill (p, m.inLatency, loop);
    m.stretch.reset();
    m.stretch.seek (m.inPtr, m.inLatency, rate);
    m.feedPos = p + m.inLatency;

    // run the stretcher through its output latency: after that its output is aligned with the position
    const int primeIn = std::max (1, (int) std::llround ((double) m.outLatency * rate));
    const int primeOut = m.outLatency;
    if (primeOut > 0 && primeIn <= m.inCap && primeOut <= m.outCap)
    {
        m.fill (m.feedPos, primeIn, loop);
        for (int c = 0; c < m.channels; ++c)
            m.outPtr[(size_t) c] = m.outBuf[(size_t) c].data();
        m.stretch.process (m.inPtr, primeIn, m.outPtr, primeOut);
        m.feedPos += primeIn;
    }

    // the stream of stretched audio starts here; its first sample is the one at the position
    for (auto& v : m.mid)
        v.assign ((size_t) Impl::sincHalf, 0.0f);   // silence in front of the first sample for the resampler taps
    m.midBase = -Impl::sincHalf;
    m.midCount = 0;
    m.midRead = 0.0;
    primed = true;
}

void LiveStretch::renderPiece (float* outL, float* outR, int n, double posStart, double rho, bool loop, double semitones)
{
    auto& m = *impl;
    const int half = Impl::sincHalf;

    // the pitch factor: a change is smooth, the resampler just reads the stretched stream at another speed
    const double f = std::pow (2.0, juce::jlimit (-12.0, 12.0, semitones) / 12.0);
    m.pitch = f;
    if (std::abs (m.tableFor - f) > 1.0e-9)
        m.buildTable (f);
    const double rate = rho / f;   // input samples per sample of the stretcher's output

    // where the stretcher is in the file: the next output of the stretcher belongs to
    //     feedPos - inLatency - outLatency * rate,
    // and the sample about to be played lies (midCount - midRead) stretcher samples earlier
    auto playedPos = [&]
    {
        return (double) m.feedPos - (double) m.inLatency - (double) m.outLatency * rate
               - ((double) m.midCount - m.midRead) * rate;
    };

    if (! primed || std::abs (playedPos() - posStart) > 12000.0)
        reseek (posStart, rho, loop);

    // stretched samples needed so that every tap of the last output sample of this piece exists
    const long long need = (long long) std::floor (m.midRead + (double) (n - 1) * f) + half + 1;
    const int g = (int) std::max<long long> (0, need - m.midCount);

    if (g > 0 && g <= m.outCap)
    {
        // a gentle servo keeps the stretcher on the position given by the song clock (rounding, tempo changes)
        const double nominal = (double) g * rate;
        const double err = posStart - playedPos();
        const double lim = 0.15 * nominal + 2.0;
        const double corr = std::max (-lim, std::min (lim, err * 0.2));
        const int nIn = std::max (1, std::min (m.inCap, (int) std::llround (nominal + corr)));

        m.fill (m.feedPos, nIn, loop);
        for (int c = 0; c < m.channels; ++c)
            m.outPtr[(size_t) c] = m.outBuf[(size_t) c].data();
        m.stretch.process (m.inPtr, nIn, m.outPtr, g);
        m.feedPos += nIn;

        for (int c = 0; c < m.channels; ++c)
            m.mid[(size_t) c].insert (m.mid[(size_t) c].end(), m.outBuf[(size_t) c].begin(), m.outBuf[(size_t) c].begin() + g);
        m.midCount += g;
    }

    // resample: output i reads the stretched stream at midRead + i * f
    const int taps = 2 * half;
    const float* tab = m.table.data();
    for (int i = 0; i < n; ++i)
    {
        const double x = m.midRead + (double) i * f;
        const long long k = (long long) std::floor (x);
        const double frac = x - (double) k;
        const double fp = frac * Impl::sincPhases;
        const int ph = std::min (Impl::sincPhases - 1, (int) fp);
        const float w1 = (float) (fp - ph);
        const float* r0 = tab + (size_t) ph * taps;
        const float* r1 = r0 + taps;

        // taps cover stream indexes k - half + 1 .. k + half; the buffer starts at midBase
        const long long first = k - half + 1 - m.midBase;
        float accL = 0.0f, accR = 0.0f;
        const float* a = m.mid[0].data();
        const float* b = m.channels > 1 ? m.mid[1].data() : a;
        if (first >= 0 && first + taps <= (long long) m.mid[0].size())
        {
            for (int t = 0; t < taps; ++t)
            {
                const float cf = r0[t] + (r1[t] - r0[t]) * w1;
                accL += cf * a[first + t];
                accR += cf * b[first + t];
            }
        }
        outL[i] += accL;
        outR[i] += accR;
    }
    m.midRead += (double) n * f;

    // drop what the resampler no longer needs
    const long long keepFrom = (long long) std::floor (m.midRead) - half;
    const long long drop = keepFrom - m.midBase;
    if (drop > 0)
    {
        for (auto& v : m.mid)
            v.erase (v.begin(), v.begin() + std::min<long long> (drop, (long long) v.size()));
        m.midBase += drop;
    }
}

void LiveStretch::render (float* outL, float* outR, int n, double posStart, double rho, bool loop, double semitones)
{
    if (! ready.load())
        return;
    for (int done = 0; done < n; done += Impl::piece)
    {
        const int m = std::min (Impl::piece, n - done);
        renderPiece (outL + done, outR + done, m, posStart + (double) done * rho, rho, loop, semitones);
    }
}
