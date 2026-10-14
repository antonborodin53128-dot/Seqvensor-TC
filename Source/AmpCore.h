#pragma once

// Core of the built-in "Amp" (guitar amplifier + cabinet), plain C++ so that it can be tested offline.
// Zero latency: no oversampling, no look-ahead, no FFT convolution. Every non-linear stage uses first-order
// anti-derivative anti-aliasing (ADAA), the cabinet is a chain of minimum-phase filters.
//
// Signal path (per channel): input trim -> pre-amp (1..4 triode-like stages: coupling high-pass, gain, asymmetric soft
// clip, Miller low-pass) -> tone stack (bass / mid / treble) -> power amp (sag + soft clip) -> presence -> cabinet -> master.

#include <algorithm>
#include <cmath>

namespace tcamp
{
    constexpr double kPi = 3.14159265358979323846;

    struct Bq
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float process (float x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return (float) y;
        }
        void reset() { z1 = z2 = 0; }
        void set (double nb0, double nb1, double nb2, double a0, double na1, double na2)
        {
            b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
        }
        void lowPass (double sr, double f, double q)
        {
            const double w = 2 * kPi * f / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
            set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
        }
        void highPass (double sr, double f, double q)
        {
            const double w = 2 * kPi * f / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
            set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
        }
        void peak (double sr, double f, double gDb, double q)
        {
            const double A = std::pow (10.0, gDb / 40.0), w = 2 * kPi * f / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
            set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
        }
        void lowShelf (double sr, double f, double gDb)
        {
            const double A = std::pow (10.0, gDb / 40.0), w = 2 * kPi * f / sr, c = std::cos (w), sn = std::sin (w);
            const double al = sn / 2 * std::sqrt (2.0), sq = 2 * std::sqrt (A) * al;
            set (A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq),
                 (A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq);
        }
        void highShelf (double sr, double f, double gDb)
        {
            const double A = std::pow (10.0, gDb / 40.0), w = 2 * kPi * f / sr, c = std::cos (w), sn = std::sin (w);
            const double al = sn / 2 * std::sqrt (2.0), sq = 2 * std::sqrt (A) * al;
            set (A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq),
                 (A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq);
        }
    };

    struct OnePole
    {
        float a = 0.0f, s = 0.0f;
        void setCutoff (double sr, double f) { a = (float) (1.0 - std::exp (-2.0 * kPi * f / sr)); }
        float lp (float x) { s += a * (x - s); return s; }
        float hp (float x) { s += a * (x - s); return x - s; }
        void reset() { s = 0.0f; }
    };

    inline float lnCosh (float x)
    {
        const float ax = std::abs (x);
        return ax + std::log1p (std::exp (-2.0f * ax)) - 0.6931472f;
    }

    // y = tanh (x + bias) - tanh (bias), first-order ADAA
    struct Clipper
    {
        float xp = 0.0f;
        float process (float x, float bias)
        {
            const float d = x - xp;
            float y;
            if (std::abs (d) < 1.0e-4f)
                y = std::tanh (0.5f * (x + xp) + bias);
            else
                y = (lnCosh (x + bias) - lnCosh (xp + bias)) / d;
            xp = x;
            return y - std::tanh (bias);
        }
        void reset() { xp = 0.0f; }
    };

    enum Model { Clean = 0, Crunch = 1, Lead = 2 };
    enum Cab { CabOff = 0, Cab1x12 = 1, Cab2x12 = 2, Cab4x12 = 3 };

    struct Params
    {
        int model = Crunch, cab = Cab4x12;
        float gain = 0.5f, bass = 0.5f, mid = 0.5f, treble = 0.5f, presence = 0.5f; // 0..1
        float masterDb = -6.0f, inputDb = 0.0f;
    };

    class Channel
    {
    public:
        static constexpr int kStages = 4;

        void prepare (double sampleRate) { sr = sampleRate; reset(); }

        void reset()
        {
            inHp.reset();
            for (int i = 0; i < kStages; ++i) { cp[i].reset(); miller[i].reset(); clip[i].reset(); }
            for (auto& b : tone) b.reset();
            powerClip.reset(); sag = 0.0f;
            pres.reset();
            for (auto& b : cabF) b.reset();
            dc = 0.0f;
        }

        void setParams (const Params& p)
        {
            cur = p;
            const bool lead = p.model == Lead, crunch = p.model == Crunch;
            nStages = lead ? 4 : (crunch ? 3 : 2);
            inHp.setCutoff (sr, lead ? 140.0 : (crunch ? 90.0 : 40.0));

            // overall pre-amp gain from the Gain knob (exponential), split over the stages
            const float g = p.gain;
            const float totalDb = lead ? (20.0f + 44.0f * g) : (crunch ? (10.0f + 36.0f * g) : (0.0f + 22.0f * g));
            float perStage[kStages];
            for (int i = 0; i < nStages; ++i)
                perStage[i] = std::pow (10.0f, totalDb / (20.0f * (float) nStages));
            for (int i = 0; i < nStages; ++i)
            {
                gainS[i] = perStage[i];
                bias[i] = lead ? 0.25f : (crunch ? 0.18f : 0.10f);
                cp[i].setCutoff (sr, i == 0 ? 25.0 : (lead ? 90.0 + 30.0 * i : 60.0));
                miller[i].setCutoff (sr, lead ? 7500.0 - 700.0 * i : (crunch ? 9000.0 : 12000.0));
            }

            // tone stack
            const double bassF = lead ? 100.0 : 120.0, midF = lead ? 750.0 : (crunch ? 650.0 : 480.0), trebF = lead ? 2600.0 : 3200.0;
            tone[0].lowShelf (sr, bassF, (p.bass - 0.5f) * 24.0f);
            tone[1].peak (sr, midF, (p.mid - 0.5f) * 20.0f, lead ? 0.9 : 0.7);
            tone[2].highShelf (sr, trebF, (p.treble - 0.5f) * 24.0f);
            pres.highShelf (sr, 4500.0, p.presence * 9.0f);

            // cabinet (minimum phase)
            switch (p.cab)
            {
                case Cab1x12:
                    cabF[0].highPass (sr, 95.0, 0.8);   cabF[1].peak (sr, 150.0, 3.0, 1.2); cabF[2].peak (sr, 3000.0, 4.0, 1.0);
                    cabF[3].peak (sr, 6000.0, -3.0, 1.5); cabF[4].lowPass (sr, 5800.0, 0.6); cabF[5].lowPass (sr, 7500.0, 0.9);
                    break;
                case Cab2x12:
                    cabF[0].highPass (sr, 80.0, 0.8);   cabF[1].peak (sr, 125.0, 4.0, 1.0); cabF[2].peak (sr, 2500.0, 3.5, 0.9);
                    cabF[3].peak (sr, 5200.0, -3.0, 1.5); cabF[4].lowPass (sr, 5200.0, 0.6); cabF[5].lowPass (sr, 6800.0, 0.9);
                    break;
                case Cab4x12:
                    cabF[0].highPass (sr, 70.0, 0.8);   cabF[1].peak (sr, 105.0, 5.0, 1.0); cabF[2].peak (sr, 2200.0, 3.0, 0.9);
                    cabF[3].peak (sr, 4200.0, -2.5, 1.4); cabF[4].lowPass (sr, 4800.0, 0.6); cabF[5].lowPass (sr, 6300.0, 0.9);
                    break;
                default: break;
            }

            // loudness compensation so that Gain changes the character, not the level
            comp = compensation (p);
            master = std::pow (10.0f, p.masterDb / 20.0f);
            inGain = std::pow (10.0f, p.inputDb / 20.0f);
            sagAttack = 1.0f - std::exp (-1.0f / (float) (sr * 0.004));
            sagRelease = 1.0f - std::exp (-1.0f / (float) (sr * 0.15));
            sagAmount = lead ? 0.55f : (crunch ? 0.45f : 0.25f);
        }

        float process (float in)
        {
            float x = inHp.hp (in * inGain) * 0.7f;
            for (int i = 0; i < nStages; ++i)
            {
                x = cp[i].hp (x);
                x = clip[i].process (x * gainS[i], bias[i]);
                x = miller[i].lp (x);
                x *= 1.0f - 0.12f * (float) i; // small loss between stages
            }
            x = tone[0].process (x);
            x = tone[1].process (x);
            x = tone[2].process (x);

            // power amp: the supply sags under load, then a push-pull soft clip
            const float lvl = std::abs (x);
            sag += (lvl - sag) * (lvl > sag ? sagAttack : sagRelease);
            x /= 1.0f + sagAmount * sag * 2.0f;
            x = powerClip.process (x * 1.6f, 0.0f) * 0.7f;
            x = pres.process (x);

            if (cur.cab != CabOff)
                for (auto& b : cabF) x = b.process (x);
            else
                x = x * 0.8f;

            dc += (x - dc) * 0.0008f;
            return (x - dc) * comp * master;
        }

        // set by the calibration table below
        static float compensation (const Params& p);

    private:
        double sr = 48000.0;
        Params cur;
        int nStages = 3;
        OnePole inHp, cp[kStages], miller[kStages];
        Clipper clip[kStages], powerClip;
        float gainS[kStages] = {}, bias[kStages] = {};
        Bq tone[3], pres, cabF[6];
        float sag = 0.0f, sagAttack = 0.01f, sagRelease = 0.001f, sagAmount = 0.4f, comp = 1.0f, master = 0.5f, inGain = 1.0f, dc = 0.0f;
    };

    // Calibrated offline with a guitar-like test signal: equal output level (about +4 dB over the input) at every Gain.
    inline float Channel::compensation (const Params& p)
    {
        static const float t[3][5] = { { 1.923f, 1.096f, 0.667f, 0.445f, 0.327f },
                                       { 1.331f, 0.642f, 0.394f, 0.322f, 0.299f },
                                       { 1.885f, 0.842f, 0.577f, 0.477f, 0.431f } };
        const float pos = std::min (1.0f, std::max (0.0f, p.gain)) * 4.0f;
        const int i = std::min (3, (int) pos);
        const auto& r = t[std::max (0, std::min (2, p.model))];
        return r[i] + (r[i + 1] - r[i]) * (pos - (float) i);
    }
}
