#pragma once

// Plain C++ cores of the time and dynamics effects of TC: compressor, 8-band equaliser, chorus and the stereo delay
// engine that the "Delay" plugin and the delay pedal of the Amp share. No JUCE, no latency (the delay lines are the
// effect itself), so everything can be tested offline.

#include "AmpCore.h"
#include "ReverbCore.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace tcfx
{
    // ------------------------------------------------------------ delay times as fractions of the beat

    struct Division
    {
        const char* name;
        double beats;   // length in quarter notes
    };

    constexpr int kDivisions = 17;

    // sorted by length: triplet (T) = 2/3 of the note, dotted (.) = 3/2 of the note
    inline const Division* divisions()
    {
        static const Division d[kDivisions] = {
            { "1/64T", 0.0625 * 2.0 / 3.0 }, { "1/32T", 0.125 * 2.0 / 3.0 }, { "1/32", 0.125 },  { "1/16T", 0.25 * 2.0 / 3.0 },
            { "1/16", 0.25 },                { "1/8T", 0.5 * 2.0 / 3.0 },    { "1/16.", 0.375 }, { "1/8", 0.5 },
            { "1/4T", 2.0 / 3.0 },           { "1/8.", 0.75 },               { "1/4", 1.0 },     { "1/2T", 2.0 * 2.0 / 3.0 },
            { "1/4.", 1.5 },                 { "1/2", 2.0 },                 { "1/2.", 3.0 },    { "1 Bar", 4.0 },
            { "2 Bar", 8.0 }
        };
        return d;
    }

    inline double divisionSeconds (int index, double bpm)
    {
        index = std::max (0, std::min (kDivisions - 1, index));
        return divisions()[index].beats * 60.0 / std::max (20.0, std::min (400.0, bpm));
    }

    // ------------------------------------------------------------ compressor (feed-forward, soft knee, linked stereo)

    class Compressor
    {
    public:
        void prepare (double sampleRate)
        {
            sr = sampleRate;
            attack = (float) (1.0 - std::exp (-1.0 / (0.003 * sr)));
            release = (float) (1.0 - std::exp (-1.0 / (0.14 * sr)));
            env = 0.0f;
            gainDb = 0.0f;
        }

        // sensitivity 0..1 (more = lower threshold = more compression), levelDb = output trim
        void setParams (float sensitivity, float levelDbIn)
        {
            threshDb = -6.0f - 34.0f * std::max (0.0f, std::min (1.0f, sensitivity));
            levelDb = levelDbIn;
            makeupDb = -threshDb * (1.0f - 1.0f / ratio) * 0.45f;
        }

        void process (float& l, float& r)
        {
            const float level = std::max (std::abs (l), std::abs (r));
            env += (level - env) * (level > env ? attack : release);
            const float envDb = 20.0f * std::log10 (std::max (env, 1.0e-6f));
            const float over = envDb - threshDb;
            float red;
            if (over <= -knee * 0.5f)
                red = 0.0f;
            else if (over >= knee * 0.5f)
                red = over * (1.0f - 1.0f / ratio);
            else
            {
                const float t = over + knee * 0.5f;
                red = (1.0f - 1.0f / ratio) * t * t / (2.0f * knee);
            }
            // smooth the gain a little so that the reduction does not follow single samples
            gainDb += (-red - gainDb) * 0.2f;
            const float g = std::pow (10.0f, (gainDb + makeupDb + levelDb) / 20.0f);
            l *= g;
            r *= g;
        }

        float reductionDb() const { return -gainDb; }

    private:
        double sr = 48000.0;
        float attack = 0.01f, release = 0.0005f, env = 0.0f, gainDb = 0.0f;
        float threshDb = -20.0f, levelDb = 0.0f, makeupDb = 0.0f;
        const float ratio = 4.0f, knee = 8.0f;
    };

    // ------------------------------------------------------------ 8-band equaliser

    class Eq8
    {
    public:
        static constexpr int kBands = 8;
        static const float* freqs()
        {
            static const float f[kBands] = { 62.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f };
            return f;
        }

        void prepare (double sampleRate)
        {
            sr = sampleRate;
            for (auto& c : ch)
                for (auto& b : c)
                    b.reset();
            have = false;
        }

        void setGains (const float* db)
        {
            bool same = have;
            for (int i = 0; i < kBands && same; ++i)
                same = same && std::abs (db[i] - cur[i]) < 1.0e-4f;
            if (same)
                return;
            have = true;
            for (int i = 0; i < kBands; ++i)
                cur[i] = db[i];
            for (auto& c : ch)
            {
                c[0].lowShelf (sr, freqs()[0], cur[0]);
                for (int i = 1; i < kBands - 1; ++i)
                    c[(size_t) i].peak (sr, freqs()[i], cur[i], 1.2);
                c[kBands - 1].highShelf (sr, freqs()[kBands - 1], cur[kBands - 1]);
            }
        }

        float process (int channel, float x)
        {
            for (auto& b : ch[channel & 1])
                x = b.process (x);
            return x;
        }

    private:
        double sr = 48000.0;
        bool have = false;
        float cur[kBands] = {};
        tcamp::Bq ch[2][kBands];
    };

    // ------------------------------------------------------------ chorus (two modulated taps, quadrature LFO)

    class Chorus
    {
    public:
        void prepare (double sampleRate)
        {
            sr = (float) sampleRate;
            for (auto& d : line)
                d.init ((int) (sr * 0.03f) + 8);
            phase = 0.0f;
        }

        // rate in Hz, depth 0..1, mix 0..1
        void setParams (float rateHz, float depth, float mixIn)
        {
            inc = 6.2831853f * std::max (0.05f, rateHz) / sr;
            depthSamples = std::max (0.0f, std::min (1.0f, depth)) * 0.0045f * sr;
            mix = std::max (0.0f, std::min (1.0f, mixIn));
        }

        void process (float& l, float& r)
        {
            const float base = 0.0085f * sr;
            line[0].write (l);
            line[1].write (r);
            const float wl = line[0].read (base + std::sin (phase) * depthSamples);
            const float wr = line[1].read (base + std::sin (phase + 1.5707963f) * depthSamples);
            phase += inc;
            if (phase > 6.2831853f)
                phase -= 6.2831853f;
            const float dry = 1.0f - 0.5f * mix;
            l = l * dry + wl * mix * 0.8f;
            r = r * dry + wr * mix * 0.8f;
        }

    private:
        float sr = 48000.0f, phase = 0.0f, inc = 0.0001f, depthSamples = 0.0f, mix = 0.0f;
        tcrev::Delay line[2];
    };

    // ------------------------------------------------------------ stereo delay engine

    enum DelayMode { Stereo = 0, PingPong = 1, Dual = 2 };

    class DelayEngine
    {
    public:
        struct Params
        {
            double timeL = 0.4, timeR = 0.4;   // seconds
            float feedback = 0.35f;            // 0..2 (200 %)
            int mode = Stereo;
            float modDepth = 0.0f;             // 0..1
            float modRate = 0.5f;              // Hz
            float hpHz = 20.0f, lpHz = 20000.0f;
            bool invertL = false, invertR = false;
        };

        static constexpr double kMaxSeconds = 9.5;

        void prepare (double sampleRate)
        {
            sr = sampleRate;
            for (auto& d : line)
                d.init ((int) (sr * (kMaxSeconds + 0.5)));
            reset();
        }

        void reset()
        {
            for (auto& d : line)
                std::fill (d.buf.begin(), d.buf.end(), 0.0f);
            for (auto& f : hp) f.reset();
            for (auto& f : lp) f.reset();
            first = true;
            phase = 0.0f;
        }

        void setParams (const Params& p)
        {
            cur = p;
            const double l = std::max (0.001, std::min (kMaxSeconds, p.timeL));
            const double r = std::max (0.001, std::min (kMaxSeconds, p.mode == Dual ? p.timeR : p.timeL));
            targetL = (float) (l * sr);
            targetR = (float) (r * sr);
            if (first)
            {
                lenL = targetL;
                lenR = targetR;
                first = false;
            }
            glide = (float) (1.0 / (0.12 * sr));
            for (auto& f : hp) f.setCutoff (sr, std::max (10.0f, std::min (0.45f * (float) sr, p.hpHz)));
            for (auto& f : lp) f.setCutoff (sr, std::max (200.0f, std::min (0.45f * (float) sr, p.lpHz)));
            useHp = p.hpHz > 21.0f;
            useLp = p.lpHz < 19000.0f;
            incMod = 6.2831853f * std::max (0.05f, p.modRate) / (float) sr;
            modSamples = std::max (0.0f, std::min (1.0f, p.modDepth)) * 0.004f * (float) sr;
        }

        // wet signal only
        void process (float inL, float inR, float& wetL, float& wetR)
        {
            lenL += (targetL - lenL) * glide * 6.0f;
            lenR += (targetR - lenR) * glide * 6.0f;
            const float mod = std::sin (phase) * modSamples;
            phase += incMod;
            if (phase > 6.2831853f)
                phase -= 6.2831853f;

            float tl = line[0].read (std::max (2.0f, lenL + mod));
            float tr = line[1].read (std::max (2.0f, lenR - mod));
            if (useHp) { tl = hp[0].hp (tl); tr = hp[1].hp (tr); }
            if (useLp) { tl = lp[0].lp (tl); tr = lp[1].lp (tr); }

            float fl = tl * cur.feedback, fr = tr * cur.feedback;
            if (cur.feedback > 0.9f)   // beyond about 100 % the repeats build up: keep them bounded
            {
                fl = std::tanh (fl);
                fr = std::tanh (fr);
            }

            if (cur.mode == PingPong)
            {
                const float m = 0.5f * (inL + inR);
                line[0].write (m + fr);
                line[1].write (fl);
            }
            else
            {
                line[0].write (inL + fl);
                line[1].write (inR + fr);
            }
            wetL = cur.invertL ? -tl : tl;
            wetR = cur.invertR ? -tr : tr;
        }

    private:
        double sr = 48000.0;
        Params cur;
        tcrev::Delay line[2];
        tcamp::OnePole hp[2], lp[2];
        float lenL = 1.0f, lenR = 1.0f, targetL = 1.0f, targetR = 1.0f, glide = 0.0001f;
        float phase = 0.0f, incMod = 0.0001f, modSamples = 0.0f;
        bool first = true, useHp = false, useLp = false;
    };
}
