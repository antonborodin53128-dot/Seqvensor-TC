#pragma once

// Pitch correction core ("autotune") with a short, bounded delay.
//
//  * pitch detector: YIN on a 12 kHz copy of the input (hop 128 samples), refined at full rate;
//    it only looks at the past, so it adds no latency of its own
//  * pitch shifter: pitch-synchronous delay line. The read head moves at ratio r = target / input pitch;
//    whenever the delay leaves a window of exactly one input period it jumps by one period (cross-faded),
//    which repeats or drops whole pitch cycles: no FFT blocks, no grains, latency = about 1.5 periods
//  * correction: nearest allowed note, retune speed, humanize (slower on held notes), flex tune (dead zone)
//
// Plain C++ (no JUCE), so it can be tested offline.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tctune
{
    struct Params
    {
        float retuneMs = 30.0f;     // 0 = instant (hard tune)
        float humanize = 10.0f;     // 0..100
        float flex = 15.0f;         // 0..100
        bool classic = false;       // classic: plain retune speed, humanize / flex are ignored
        bool hq = false;
        float refHz = 440.0f;
        std::uint16_t mask = 0x0FFF;    // allowed pitch classes, bit 0 = C
        float rangeLowHz = 70.0f, rangeHighHz = 900.0f;
        int voices = 0;             // extra "choir" voices 0..6
        float spread = 30.0f;       // 0..100: detune, timing and drift of those voices
    };

    class Core
    {
    public:
        static constexpr int ringSize = 16384, ringMask = ringSize - 1;
        static constexpr int hop = 128;

        void prepare (double sampleRate)
        {
            sr = sampleRate;
            decim = std::max (1, (int) std::lround (sr / 12000.0));
            fsd = sr / decim;
            for (auto& r : ring)
                std::fill (r.begin(), r.end(), 0.0f);
            std::fill (mono.begin(), mono.end(), 0.0f);
            std::fill (dec.begin(), dec.end(), 0.0f);
            wpos = 0;
            monoPos = decPos = 0;
            decAcc = 0.0f;
            decCount = 0;
            hopCount = 0;
            for (int v = 0; v < maxVoices; ++v)
            {
                Voice& vo = voices[v];
                vo = Voice {};
                vo.h[0].d = vo.h[1].d = nominalDelay();
                // fixed personality of each voice: detune sign / size, timing, drift speed, pan
                static const float det[maxVoices] = { 0.0f, -1.0f, 0.8f, -0.55f, 1.0f, -0.3f, 0.6f };
                static const float tim[maxVoices] = { 0.0f, 0.7f, 1.0f, 0.35f, 0.85f, 0.55f, 0.2f };
                static const float lfo[maxVoices] = { 0.0f, 0.37f, 0.53f, 0.71f, 0.43f, 0.61f, 0.89f };
                static const float pan[maxVoices] = { 0.0f, -0.8f, 0.8f, -0.4f, 0.4f, -1.0f, 1.0f };
                vo.detune = det[v];
                vo.timing = tim[v];
                vo.lfoRate = lfo[v];
                vo.lfoPhase = 0.17f * (float) v;
                vo.pan = pan[v];
            }
            period = (float) (sr / 150.0);
            haveTarget = false;
            sCur = sGoal = 0.0f;
            voicedRun = unvoicedRun = 0;
            hasPitch = false;
            ratio = ratioTarget = 1.0f;
            lastF0 = 0.0f;
            medCount = 0;
            heldTime = 0.0f;
            outMidi.store (0.0f);
            outVoiced.store (false);
            outTarget.store (-1);
            outShiftCents.store (0.0f);
            outInCents.store (0.0f);
        }

        // reported latency, samples: typical (150 Hz voice); the real one follows the pitch period
        int getTypicalLatency (bool hqMode) const
        {
            const float f = fadeLen (hqMode);
            return (int) std::lround (f + 8.0f + sr / 150.0 * 0.5);
        }

        void setParams (const Params& p) { params = p; }

        void process (float* l, float* r, int n)
        {
            const int nv = std::max (0, std::min (maxVoices - 1, params.voices));
            const float sp = params.spread * 0.01f;
            const float voiceGain = nv > 0 ? 0.8f / std::sqrt ((float) nv) : 0.0f;
            const float mainGain = nv > 0 ? 1.0f / std::sqrt (1.0f + 0.25f * (float) nv) : 1.0f;
            const float fl = fadeLen (params.hq);

            for (int i = 0; i < n; ++i)
            {
                const float xl = l[i], xr = r ? r[i] : l[i];
                feed (0.5f * (xl + xr));

                ring[0][(size_t) (wpos & ringMask)] = xl;
                ring[1][(size_t) (wpos & ringMask)] = xr;
                ++wpos;

                if (++hopCount >= hop)
                {
                    hopCount = 0;
                    analyse();
                }
                ratio += (ratioTarget - ratio) * rampCoef;

                float outL = 0.0f, outR = 0.0f;
                for (int v = 0; v <= nv; ++v)
                {
                    Voice& vo = voices[v];
                    float rho = ratio;
                    float extra = 0.0f;
                    if (v > 0)
                    {
                        // slow drift + fixed detune, in cents; timing offset in samples
                        vo.lfoPhase += vo.lfoRate / (float) sr;
                        if (vo.lfoPhase > 1.0f)
                            vo.lfoPhase -= 1.0f;
                        const float cents = sp * (28.0f * vo.detune + 14.0f * std::sin (6.2831853f * vo.lfoPhase));
                        rho *= std::exp2 (cents / 1200.0f);
                        extra = sp * vo.timing * (float) (sr * 0.028);
                    }

                    vo.h[0].d += 1.0f - rho;
                    vo.h[1].d += 1.0f - rho;
                    keepWindow (vo, extra, fl);
                    if (vo.fade < 1.0f)
                        vo.fade = std::min (1.0f, vo.fade + 1.0f / fl);

                    const float wl = read (0, vo.h[0]) * vo.fade + read (0, vo.h[1]) * (1.0f - vo.fade);
                    const float wr = r != nullptr ? read (1, vo.h[0]) * vo.fade + read (1, vo.h[1]) * (1.0f - vo.fade) : wl;
                    if (v == 0)
                    {
                        outL += wl * mainGain;
                        outR += wr * mainGain;
                    }
                    else
                    {
                        const float m = 0.5f * (wl + wr);
                        const float pl = 0.5f * (1.0f - vo.pan * 0.8f * std::max (0.2f, sp)), pr = 1.0f - pl;
                        outL += (wl * 0.5f + m * pl) * voiceGain * 0.95f;
                        outR += (wr * 0.5f + m * pr) * voiceGain * 0.95f;
                    }
                }
                l[i] = outL;
                if (r != nullptr)
                    r[i] = outR;
            }
        }

        // meters (read from the UI thread)
        std::atomic<float> outMidi { 0.0f };          // detected pitch, semitones (midi, relative to reference)
        std::atomic<bool> outVoiced { false };
        std::atomic<int> outTarget { -1 };            // target pitch class 0..11, -1 = none
        std::atomic<float> outShiftCents { 0.0f };
        std::atomic<float> outInCents { 0.0f };       // detected pitch minus target, cents

    private:
        static constexpr int maxVoices = 7;
        struct Head { float d = 0.0f; };
        struct Voice
        {
            Head h[2];
            float fade = 1.0f, detune = 0.0f, timing = 0.0f, lfoRate = 0.0f, lfoPhase = 0.0f, pan = 0.0f;
        };

        float fadeLen (bool hqMode) const { return (float) (sr * (hqMode ? 0.003 : 0.0015)); }
        float nominalDelay() const { return fadeLen (params.hq) + 8.0f + (float) (sr / 150.0) * 0.5f; }

        float read (int ch, const Head& h) const
        {
            const float pos = (float) (wpos - 1) - h.d;
            const int i0 = (int) std::floor (pos);
            const float f = pos - (float) i0;
            const auto& b = ring[ch];
            const float y0 = b[(size_t) ((i0 - 1) & ringMask)], y1 = b[(size_t) (i0 & ringMask)],
                        y2 = b[(size_t) ((i0 + 1) & ringMask)], y3 = b[(size_t) ((i0 + 2) & ringMask)];
            const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            return ((c3 * f + c2) * f + c1) * f + y1;
        }

        // the delay lives in a window of exactly one input period
        void keepWindow (Voice& vo, float extra, float fl)
        {
            const float lo = fl + 8.0f + extra;
            const float hi = lo + period;
            float& d = vo.h[0].d;
            if (d < lo || d > hi)
            {
                const float p = std::max (period, 16.0f);
                const float k = d < lo ? std::ceil ((lo - d) / p) : -std::ceil ((d - hi) / p);
                vo.h[1].d = d;                  // the old head fades out
                vo.h[0].d = d + k * p;
                vo.fade = 0.0f;
                vo.h[0].d = std::min (std::max (vo.h[0].d, 2.0f), (float) (ringSize - 64));
            }
            vo.h[1].d = std::min (std::max (vo.h[1].d, 2.0f), (float) (ringSize - 64));
        }

        // ---------------------------------------------------------------- analysis
        void feed (float x)
        {
            mono[(size_t) (monoPos & monoMask)] = x;
            ++monoPos;
            decAcc += x;
            if (++decCount >= decim)
            {
                dec[(size_t) (decPos & decMask)] = decAcc / (float) decim;
                ++decPos;
                decAcc = 0.0f;
                decCount = 0;
            }
        }

        float decAt (int back) const { return dec[(size_t) ((decPos - 1 - back) & decMask)]; }
        float monoAt (int back) const { return mono[(size_t) ((monoPos - 1 - back) & monoMask)]; }

        // YIN; returns period in full-rate samples, 0 = unvoiced. conf = 1 - dip.
        float detect (float& conf) const
        {
            const int W = params.hq ? 320 : 224;
            const int tauMin = std::max (4, (int) (fsd / params.rangeHighHz));
            const int tauMax = std::min (decMask / 4, (int) (fsd / params.rangeLowHz));
            if (tauMax <= tauMin + 2)
                return 0.0f;

            // energy gate
            double e = 0.0;
            for (int j = 0; j < 512; ++j)
            {
                const float v = monoAt (j);
                e += (double) v * v;
            }
            if (e / 512.0 < 1.0e-7)   // about -70 dB
                return 0.0f;

            static thread_local std::vector<float> d;
            d.assign ((size_t) tauMax + 2, 0.0f);
            for (int tau = 1; tau <= tauMax + 1; ++tau)
            {
                float s = 0.0f;
                for (int j = 0; j < W; ++j)
                {
                    const float diff = decAt (j) - decAt (j + tau);
                    s += diff * diff;
                }
                d[(size_t) tau] = s;
            }
            // cumulative mean normalised difference
            float run = 0.0f;
            d[0] = 1.0f;
            for (int tau = 1; tau <= tauMax + 1; ++tau)
            {
                run += d[(size_t) tau];
                d[(size_t) tau] = run > 1.0e-12f ? d[(size_t) tau] * (float) tau / run : 1.0f;
            }

            int best = -1;
            for (int tau = tauMin; tau <= tauMax; ++tau)
                if (d[(size_t) tau] < 0.16f)
                {
                    while (tau + 1 <= tauMax && d[(size_t) (tau + 1)] < d[(size_t) tau])
                        ++tau;
                    best = tau;
                    break;
                }
            if (best < 0)
            {
                // no clear dip: take the global minimum if it is still decent
                float m = 1.0f;
                for (int tau = tauMin; tau <= tauMax; ++tau)
                    if (d[(size_t) tau] < m)
                    {
                        m = d[(size_t) tau];
                        best = tau;
                    }
                if (m > 0.3f)
                    return 0.0f;
            }
            conf = 1.0f - d[(size_t) best];

            // parabolic interpolation on the coarse grid
            float tauF = (float) best;
            if (best > tauMin && best < tauMax)
            {
                const float a = d[(size_t) best - 1], b = d[(size_t) best], c = d[(size_t) best + 1];
                const float den = a - 2.0f * b + c;
                if (std::abs (den) > 1.0e-9f)
                    tauF += 0.5f * (a - c) / den;
            }
            const float coarse = tauF * (float) decim;

            // refine at the full rate around the coarse value
            const int Wf = 640;
            const int lo = std::max (8, (int) std::floor (coarse) - decim - 1), hi = (int) std::ceil (coarse) + decim + 1;
            float bestVal = 1.0e30f;
            int bestLag = lo;
            static thread_local std::vector<float> val;
            val.assign ((size_t) (hi - lo + 1), 0.0f);
            for (int lag = lo; lag <= hi; ++lag)
            {
                float s = 0.0f, en = 1.0e-9f;
                for (int j = 0; j < Wf; ++j)
                {
                    const float a = monoAt (j), b = monoAt (j + lag);
                    s += (a - b) * (a - b);
                    en += a * a + b * b;
                }
                const float v = s / en;
                val[(size_t) (lag - lo)] = v;
                if (v < bestVal)
                {
                    bestVal = v;
                    bestLag = lag;
                }
            }
            float lagF = (float) bestLag;
            if (bestLag > lo && bestLag < hi)
            {
                const float a = val[(size_t) (bestLag - lo - 1)], b = val[(size_t) (bestLag - lo)], c = val[(size_t) (bestLag - lo + 1)];
                const float den = a - 2.0f * b + c;
                if (den > 1.0e-12f)
                    lagF += 0.5f * (a - c) / den;
            }
            return lagF;
        }

        void analyse()
        {
            float conf = 0.0f;
            const float per = detect (conf);
            const float hopSec = (float) (hop / sr);

            if (per > 0.0f)
            {
                // median of the last three estimates removes single octave / outlier errors
                med[(size_t) (medCount++ % 3)] = per;
                float v[3] = { med[0], med[1], med[2] };
                float use = per;
                if (medCount >= 3)
                {
                    std::sort (v, v + 3);
                    use = v[1];
                }
                ++voicedRun;
                unvoicedRun = 0;
                if (voicedRun >= 2)
                {
                    period = use;
                    lastF0 = (float) (sr / use);
                    hasPitch = true;
                }
            }
            else
            {
                voicedRun = 0;
                ++unvoicedRun;
                medCount = 0;
            }

            if (unvoicedRun > 3)
                hasPitch = false;
            const bool active = hasPitch && lastF0 > 0.0f;

            const float tauMs = std::max (0.0f, params.retuneMs);
            float goal = 0.0f;
            float tc = 0.03f;

            if (active)
            {
                const float midi = 69.0f + 12.0f * std::log2 (lastF0 / params.refHz) + 0.0f;
                // nearest allowed note (with a little hysteresis around the current target)
                int bestNote = -1;
                float bestDist = 1.0e9f;
                const int base = (int) std::lround (midi);
                for (int k = base - 7; k <= base + 7; ++k)
                {
                    const int pc = ((k % 12) + 12) % 12;
                    if (! (params.mask & (1u << pc)))
                        continue;
                    float dist = std::abs ((float) k - midi);
                    if (haveTarget && k == targetNote)
                        dist -= 0.12f;
                    if (dist < bestDist)
                    {
                        bestDist = dist;
                        bestNote = k;
                    }
                }

                if (bestNote >= 0)
                {
                    if (! haveTarget || bestNote != targetNote)
                        heldTime = 0.0f;
                    else
                        heldTime += hopSec;
                    targetNote = bestNote;
                    haveTarget = true;

                    float err = (float) bestNote - midi;      // shift needed, semitones
                    if (! params.classic)
                    {
                        const float tol = params.flex * 0.01f * 0.5f;   // up to 50 cents of free pitch
                        err = err > tol ? err - tol : (err < -tol ? err + tol : 0.0f);
                    }
                    goal = err;
                    tc = tauMs * 0.001f;
                    if (! params.classic)
                        tc *= 1.0f + params.humanize * 0.01f * 6.0f * std::min (1.0f, heldTime / 0.6f);

                    outMidi.store (midi);
                    outTarget.store (((bestNote % 12) + 12) % 12);
                    outInCents.store (-(float) (bestNote - midi) * 100.0f);
                }
                else
                {
                    haveTarget = false;
                    outTarget.store (-1);
                }
            }
            else
            {
                haveTarget = false;
                heldTime = 0.0f;
                outTarget.store (-1);
            }
            outVoiced.store (active);

            const float a = tc <= 1.0e-4f ? 1.0f : 1.0f - std::exp (-hopSec / tc);
            sCur += (goal - sCur) * a;
            ratioTarget = std::exp2 (sCur / 12.0f);
            // per-sample ramp towards the new ratio over one hop
            rampCoef = 1.0f - std::exp (-1.0f / (0.5f * hop));
            outShiftCents.store (sCur * 100.0f);
        }

        // ---------------------------------------------------------------- state
        Params params;
        double sr = 48000.0, fsd = 12000.0;
        int decim = 4;

        std::vector<float> ring[2] = { std::vector<float> (ringSize, 0.0f), std::vector<float> (ringSize, 0.0f) };
        long long wpos = 0;

        static constexpr int monoMask = 4095, decMask = 2047;
        std::vector<float> mono = std::vector<float> (monoMask + 1, 0.0f), dec = std::vector<float> (decMask + 1, 0.0f);
        long long monoPos = 0, decPos = 0;
        float decAcc = 0.0f;
        int decCount = 0, hopCount = 0;

        Voice voices[maxVoices];
        float period = 320.0f;
        float ratio = 1.0f, ratioTarget = 1.0f, rampCoef = 0.01f;

        bool haveTarget = false;
        int targetNote = 0;
        float sCur = 0.0f, sGoal = 0.0f;
        bool hasPitch = false;
        int voicedRun = 0, unvoicedRun = 0;
        float lastF0 = 0.0f;
        float med[3] = { 0, 0, 0 };
        int medCount = 0;
        float heldTime = 0.0f;
    };
}
