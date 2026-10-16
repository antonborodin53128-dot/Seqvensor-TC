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

    // Six amplifiers: two clean, two crunch, two high gain. Each one is a "voicing": the number of gain stages, how the
    // gain and the tone controls behave, and the fixed colour of its tone stack and power section.
    enum Model { TwnClean = 0, RldClean = 1, MrsCrunch = 2, VxCrunch = 3, PvyDist = 4, MbDist = 5, kModels = 6 };
    enum Cab { CabOff = 0, Cab1x12 = 1, Cab2x12 = 2, Cab4x12 = 3 };

    struct Voicing
    {
        int stages;               // gain stages (1..4)
        float inHpHz;             // input high-pass: how tight the low end is
        float gainBaseDb, gainSpanDb;   // total pre-amp gain from Gain = 0 .. 1
        float bias;               // asymmetry of the clipping (even harmonics)
        float millerHz, millerStep;     // high-frequency roll-off of the first stage, and how much lower each next stage is
        float cp0Hz, cpHz, cpStep;      // coupling high-pass of the first stage, of the others (+ step per stage)
        float bassHz, midHz, trebleHz, midQ;
        float fixedMidDb, fixedTrebleDb; // colour of the tone stack that the knobs do not remove
        float sag;                // how much the power supply sags under load
        float powerDrive;         // how hard the power section is pushed
    };

    inline const Voicing& voicing (int model)
    {
        static const Voicing v[kModels] = {
            // TWN Clean: a big, bright, scooped clean amp with a lot of headroom
            { 2,  40.0f,  0.0f, 22.0f, 0.06f, 13000.0f,    0.0f, 25.0f,  60.0f,  0.0f, 100.0f,  400.0f, 3500.0f, 0.7f, -3.0f, 1.5f, 0.12f, 1.3f },
            // RLD Clean: solid-state clean, glassy and stiff (no sag, symmetric)
            { 2,  50.0f,  0.0f, 20.0f, 0.00f, 14000.0f,    0.0f, 25.0f,  60.0f,  0.0f, 120.0f,  800.0f, 4000.0f, 0.6f,  0.0f, 2.0f, 0.00f, 1.1f },
            // MRS Crunch: mid-forward British crunch
            { 3,  90.0f, 10.0f, 36.0f, 0.18f,  9000.0f,    0.0f, 25.0f,  60.0f,  0.0f, 120.0f,  650.0f, 3200.0f, 0.7f,  0.0f, 0.0f, 0.45f, 1.6f },
            // VX Crunch: chimey, upper-mid honk, EL84 breakup
            { 3,  70.0f,  8.0f, 34.0f, 0.12f, 11000.0f,    0.0f, 25.0f,  60.0f,  0.0f, 110.0f, 1100.0f, 3800.0f, 0.8f,  0.0f, 3.0f, 0.50f, 1.9f },
            // PVY Dist: tight, aggressive high gain
            { 4, 140.0f, 20.0f, 44.0f, 0.25f,  7500.0f,  700.0f, 25.0f,  90.0f, 30.0f, 100.0f,  750.0f, 2600.0f, 0.9f,  0.0f, 1.0f, 0.30f, 1.6f },
            // MB Dist: heavy, scooped, spongy high gain
            { 4, 110.0f, 18.0f, 46.0f, 0.30f,  6500.0f,  600.0f, 25.0f,  90.0f, 25.0f,  90.0f,  500.0f, 2400.0f, 1.0f, -5.0f, 0.0f, 0.75f, 1.8f }
        };
        return v[std::max (0, std::min (kModels - 1, model))];
    }

    struct Params
    {
        int model = MrsCrunch, cab = Cab4x12;
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
            const Voicing& v = voicing (p.model);
            nStages = v.stages;
            inHp.setCutoff (sr, v.inHpHz);

            // overall pre-amp gain from the Gain knob (exponential), split over the stages
            const float g = p.gain;
            const float totalDb = v.gainBaseDb + v.gainSpanDb * g;
            const float stageGain = std::pow (10.0f, totalDb / (20.0f * (float) nStages));
            for (int i = 0; i < nStages; ++i)
            {
                gainS[i] = stageGain;
                bias[i] = v.bias;
                cp[i].setCutoff (sr, i == 0 ? v.cp0Hz : v.cpHz + v.cpStep * (float) i);
                miller[i].setCutoff (sr, std::max (3000.0f, v.millerHz - v.millerStep * (float) i));
            }

            // tone stack
            tone[0].lowShelf (sr, v.bassHz, (p.bass - 0.5f) * 24.0f);
            tone[1].peak (sr, v.midHz, (p.mid - 0.5f) * 20.0f + v.fixedMidDb, v.midQ);
            tone[2].highShelf (sr, v.trebleHz, (p.treble - 0.5f) * 24.0f + v.fixedTrebleDb);
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
            sagAmount = v.sag;
            powerDrive = v.powerDrive;
        }

        // pre-amp and tone stack (the part before the effects loop of a real amp)
        float preamp (float in)
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
            return x;
        }

        // power amp, presence, cabinet and master (the part after the effects loop)
        float power (float x)
        {
            // the supply sags under load, then a push-pull soft clip
            const float lvl = std::abs (x);
            sag += (lvl - sag) * (lvl > sag ? sagAttack : sagRelease);
            x /= 1.0f + sagAmount * sag * 2.0f;
            x = powerClip.process (x * powerDrive, 0.0f) * 0.7f * (1.6f / powerDrive);
            x = pres.process (x);

            if (cur.cab != CabOff)
                for (auto& b : cabF) x = b.process (x);
            else
                x = x * 0.8f;

            dc += (x - dc) * 0.0008f;
            return (x - dc) * comp * master;
        }

        float process (float in) { return power (preamp (in)); }

        // set by the calibration table below
        static float compensation (const Params& p);

    private:
        double sr = 48000.0;
        float powerDrive = 1.6f;
        Params cur;
        int nStages = 3;
        OnePole inHp, cp[kStages], miller[kStages];
        Clipper clip[kStages], powerClip;
        float gainS[kStages] = {}, bias[kStages] = {};
        Bq tone[3], pres, cabF[6];
        float sag = 0.0f, sagAttack = 0.01f, sagRelease = 0.001f, sagAmount = 0.4f, comp = 1.0f, master = 0.5f, inGain = 1.0f, dc = 0.0f;
    };

    // Calibrated offline with a guitar-like test signal: equal output level (the same for all six amps) at every Gain.
    inline float Channel::compensation (const Params& p)
    {
        static const float t[kModels][5] = { { 2.801f, 1.535f, 0.875f, 0.536f, 0.362f },
                                             { 2.400f, 1.370f, 0.802f, 0.493f, 0.328f },
                                             { 1.483f, 0.697f, 0.415f, 0.300f, 0.253f },
                                             { 1.688f, 0.814f, 0.482f, 0.341f, 0.279f },
                                             { 1.474f, 0.624f, 0.370f, 0.284f, 0.256f },
                                             { 2.476f, 1.010f, 0.589f, 0.454f, 0.411f } };
        const float pos = std::min (1.0f, std::max (0.0f, p.gain)) * 4.0f;
        const int i = std::min (3, (int) pos);
        const auto& r = t[std::max (0, std::min (kModels - 1, p.model))];
        return r[i] + (r[i + 1] - r[i]) * (pos - (float) i);
    }
}
