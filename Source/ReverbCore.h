#pragma once

// Reverb engine of the built-in "Reverb" (plain C++, no JUCE, so it can be tested offline).
// Stereo pre-delay -> early reflections -> input diffusion (4 all-passes per side, the last two modulated) ->
// 8-line feedback delay network (Hadamard matrix, damping in the loop, chorused delay modulation) -> wet output.
// It adds no latency: the only delay is the pre-delay that the user asks for.
// Modes follow the families of the classic hardware reverbs: Hall (large, dense, lush), Plate (very diffuse,
// bright, fast build-up), Room (small, darker, clear early reflections).

#include <algorithm>
#include <cmath>
#include <vector>

namespace tcrev
{
    struct Delay
    {
        std::vector<float> buf;
        int mask = 0, w = 0;

        void init (int minLen)
        {
            int n = 1;
            while (n < minLen + 8)
                n <<= 1;
            buf.assign ((size_t) n, 0.0f);
            mask = n - 1;
            w = 0;
        }
        void write (float x) { buf[(size_t) w] = x; w = (w + 1) & mask; }
        // d >= 1 samples, fractional (linear interpolation)
        float read (float d) const
        {
            const int i = (int) d;
            const float f = d - (float) i;
            const float a = buf[(size_t) ((w - 1 - i) & mask)], b = buf[(size_t) ((w - 2 - i) & mask)];
            return a + (b - a) * f;
        }
    };

    struct Preset
    {
        float scale;      // length of the delay network relative to the base lengths
        float apGain;     // diffusion all-pass coefficient
        float dampHz;     // loop damping (high frequencies die faster)
        float modMs;      // modulation depth of the network lines
        float modRate;    // base modulation rate in Hz
        float erLevel;    // early reflections
        float erScale;    // early reflections time scale
        float inject;     // how much of the diffused input enters the network
    };

    inline const Preset& preset (int mode)
    {
        static const Preset p[3] = {
            { 2.1f, 0.70f,  5200.0f, 0.55f, 0.45f, 0.35f, 1.30f, 0.55f }, // Hall
            { 1.3f, 0.78f, 12000.0f, 0.40f, 0.95f, 0.00f, 1.00f, 0.60f }, // Plate
            { 1.7f, 0.62f,  3800.0f, 0.25f, 0.70f, 0.85f, 0.65f, 0.50f }  // Room
        };
        return p[std::max (0, std::min (2, mode))];
    }

    class Core
    {
    public:
        static constexpr int N = 8;

        void prepare (double sampleRate)
        {
            sr = (float) sampleRate;
            const float k = sr / 44100.0f;
            for (int i = 0; i < N; ++i)
            {
                lines[i].init ((int) (base[i] * 2.4f * k) + 400);
                damp[i] = 0.0f;
                phase[i] = (float) i * 0.79f;
                lap[i].init ((int) (lapBase[i] * 2.0f * k) + 64);
            }
            for (int i = 0; i < 8; ++i)
                ap[i].init ((int) (apBase[i] * k) + 64);
            pre[0].init ((int) (sr * 0.5f) + 8);
            pre[1].init ((int) (sr * 0.5f) + 8);
            er[0].init ((int) (sr * 0.12f));
            er[1].init ((int) (sr * 0.12f));
            reset();
        }

        void reset()
        {
            for (auto& d : lines) std::fill (d.buf.begin(), d.buf.end(), 0.0f);
            for (auto& d : lap) std::fill (d.buf.begin(), d.buf.end(), 0.0f);
            for (auto& d : ap) std::fill (d.buf.begin(), d.buf.end(), 0.0f);
            for (auto& d : pre) std::fill (d.buf.begin(), d.buf.end(), 0.0f);
            for (auto& d : er) std::fill (d.buf.begin(), d.buf.end(), 0.0f);
            for (int i = 0; i < N; ++i) damp[i] = 0.0f;
            dcL = dcR = 0.0f;
            first = true;
        }

        // called once per block (cheap)
        void setParams (int mode, float preDelayMs, float decaySec)
        {
            const Preset& p = preset (mode);
            const float k = sr / 44100.0f;
            for (int i = 0; i < N; ++i)
                lenTarget[i] = base[i] * p.scale * k;
            if (first)
            {
                for (int i = 0; i < N; ++i) lenCur[i] = lenTarget[i];
                preCur = std::max (1.0f, sr * 0.001f * preDelayMs);
                first = false;
            }
            curPreset = p;
            dampCoef = std::exp (-2.0f * 3.14159265f * p.dampHz / sr);
            decay = std::max (0.1f, decaySec);
            preTarget = std::max (1.0f, sr * 0.001f * preDelayMs);
            // wet level: the energy of a diffuse tail grows with its length, so compensate a bit
            outGain = 0.30f * levelTrim[std::max (0, std::min (2, mode))] * std::pow (2.5f / decay, 0.5f);
        }

        void process (float inL, float inR, float& wetL, float& wetR)
        {
            const Preset& p = curPreset;
            preCur += (preTarget - preCur) * 0.002f;

            pre[0].write (inL);
            pre[1].write (inR);
            const float xl0 = pre[0].read (preCur), xr0 = pre[1].read (preCur);

            // early reflections (taps on the pre-delayed signal)
            er[0].write (xl0);
            er[1].write (xr0);
            float erL = 0.0f, erR = 0.0f;
            if (p.erLevel > 0.0f)
            {
                static const float tl[6] = { 7.0f, 17.0f, 29.0f, 41.0f, 53.0f, 67.0f };
                static const float tr[6] = { 11.0f, 23.0f, 35.0f, 47.0f, 59.0f, 73.0f };
                static const float tg[6] = { 0.8f, 0.6f, 0.5f, 0.38f, 0.28f, 0.2f };
                for (int t = 0; t < 6; ++t)
                {
                    erL += tg[t] * er[0].read (tl[t] * p.erScale * 0.001f * sr);
                    erR += tg[t] * er[1].read (tr[t] * p.erScale * 0.001f * sr);
                }
                erL *= p.erLevel * 0.35f;
                erR *= p.erLevel * 0.35f;
            }

            // input diffusion
            const float modAp = p.modMs * 0.001f * sr * 0.5f;
            float xl = xl0, xr = xr0;
            for (int s = 0; s < 4; ++s)
            {
                const float l = apBase[s] * (sr / 44100.0f), r = apBase[s + 4] * (sr / 44100.0f);
                const float m = s >= 2 ? std::sin (apPhase[s]) * modAp : 0.0f;
                xl = allpass (ap[s], xl, l + m, p.apGain);
                xr = allpass (ap[s + 4], xr, r - m, p.apGain);
                apPhase[s] += 6.2831853f * (0.31f + 0.11f * (float) s) * p.modRate / sr;
                if (apPhase[s] > 6.2831853f) apPhase[s] -= 6.2831853f;
            }

            // feedback delay network
            const float modLine = p.modMs * 0.001f * sr;
            float f[N], tap[N];
            for (int i = 0; i < N; ++i)
            {
                lenCur[i] += (lenTarget[i] - lenCur[i]) * 0.0003f;
                const float mod = std::sin (phase[i]) * modLine;
                phase[i] += 6.2831853f * p.modRate * (0.6f + 0.13f * (float) i) / sr;
                if (phase[i] > 6.2831853f) phase[i] -= 6.2831853f;
                tap[i] = lines[i].read (lenCur[i] + mod + 2.0f);
                damp[i] = tap[i] * (1.0f - dampCoef) + damp[i] * dampCoef;
                const float g = std::pow (10.0f, -3.0f * (lenCur[i] + lapBase[i] * p.scale * sr / 44100.0f) / (sr * decay * decayFix));
                // an all-pass inside the loop makes the tail denser and smears the resonances of the network
                const float lm = std::sin (phase[i] * 1.7f + 1.0f) * modLine * 0.5f;
                f[i] = allpass (lap[i], damp[i], lapBase[i] * p.scale * sr / 44100.0f + lm, 0.5f) * g;
            }
            for (int h = 1; h < N; h <<= 1)
                for (int i = 0; i < N; i += h << 1)
                    for (int j = i; j < i + h; ++j)
                    {
                        const float a = f[j], b = f[j + h];
                        f[j] = a + b;
                        f[j + h] = a - b;
                    }
            float wl = 0.0f, wr = 0.0f;
            for (int i = 0; i < N; ++i)
            {
                const float inj = (i & 1 ? xr : xl) * p.inject;
                lines[i].write (inj + f[i] * 0.35355339f);
                wl += (i & 1 ? -damp[i] : damp[i]);
                wr += ((i >> 1) & 1 ? -damp[i] : damp[i]);
            }
            wl = wl * outGain + erL;
            wr = wr * outGain + erR;

            // keep DC out of the tail
            dcL += (wl - dcL) * 0.0015f;
            dcR += (wr - dcR) * 0.0015f;
            wetL = wl - dcL;
            wetR = wr - dcR;
        }

    private:
        static float allpass (Delay& d, float x, float len, float g)
        {
            const float del = d.read (len);
            const float w = x + g * del;
            d.write (w);
            return del - g * w;
        }

        float sr = 48000.0f, decayFix = 1.12f, decay = 2.5f, dampCoef = 0.5f, outGain = 0.3f, preCur = 1.0f, preTarget = 1.0f;
        float dcL = 0.0f, dcR = 0.0f;
        bool first = true;
        Preset curPreset = preset (0);
        Delay lines[N], lap[N], ap[8], pre[2], er[2];
        float lenCur[N] = {}, lenTarget[N] = {}, damp[N] = {}, phase[N] = {}, apPhase[4] = {};
        static constexpr float base[N] = { 1117, 1327, 1559, 1783, 2029, 2239, 2477, 2711 };
        static constexpr float levelTrim[3] = { 1.6f, 1.05f, 1.35f };
        static constexpr float lapBase[N] = { 89, 97, 107, 127, 139, 149, 163, 179 };
        static constexpr float apBase[8] = { 113, 163, 241, 337, 127, 181, 263, 359 };
    };
}
