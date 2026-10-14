#pragma once

// Core of the built-in "808 Overdrive" (a model of the classic mid-hump overdrive pedal circuit), plain C++ so that it
// can be tested offline. Zero latency: no oversampling and no look-ahead. The diode clipper uses first-order
// anti-derivative anti-aliasing (ADAA), which keeps the aliasing low at a delay of half a sample only.
//
// Circuit model:  y = x + clip( (H(s) - 1) * x ), where the op-amp stage has
//   H(s) = 1 + Zf / Zg,  Zf = Rd / (1 + s Rd Cf),  Zg = R1 + 1 / (s C1),  Rd = 51k + Drive pot,
//   R1 = 4.7k, C1 = 47 nF (720 Hz mid hump), Cf = 51 pF, followed by the diode pair (soft clip) and the tone low-pass.

#include <algorithm>
#include <cmath>

namespace tcod
{
    struct Biquad
    {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        float process (float x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return (float) y;
        }
        void reset() { z1 = z2 = 0; }
    };

    class Channel
    {
    public:
        // drive 0..1
        void setDrive (double sr, float drive)
        {
            const double Rd = 51.0e3 + 500.0e3 * (double) drive * (double) drive;
            const double R1 = 4.7e3, C1 = 47.0e-9, Cf = 51.0e-12;
            const double a = Rd * C1, d1 = Rd * Cf + R1 * C1, d2 = Rd * Cf * R1 * C1;
            const double K = 2.0 * sr;
            const double a0 = 1.0 + d1 * K + d2 * K * K;
            stage.b0 = a * K / a0;
            stage.b1 = 0.0;
            stage.b2 = -a * K / a0;
            stage.a1 = (2.0 - 2.0 * d2 * K * K) / a0;
            stage.a2 = (1.0 - d1 * K + d2 * K * K) / a0;

            // Loudness compensation: the output level stays the same (at a normal playing level) from Drive 0 to 100,
            // so Drive only changes the amount of overdrive. Measured offline (equal RMS for a 0.25 sine and a speech-like noise).
            static const float table[11] = { 0.338f, 0.330f, 0.317f, 0.307f, 0.2985f, 0.2935f, 0.2885f, 0.286f, 0.283f, 0.280f, 0.2785f };
            const float pos = std::min (1.0f, std::max (0.0f, drive)) * 10.0f;
            const int i = std::min (9, (int) pos);
            comp = table[i] + (table[i + 1] - table[i]) * (pos - (float) i);
        }

        // tone 0..1: low-pass from 720 Hz (dark) to about 11 kHz (open)
        void setTone (double sr, float tone)
        {
            const double fc = std::min (0.45 * sr, 720.0 * std::pow (2.0, 4.0 * (double) tone));
            const double k = std::exp (-2.0 * 3.14159265358979 * fc / sr);
            toneA = (float) (1.0 - k);
            toneK = (float) k;
        }

        float process (float in)
        {
            constexpr float inScale = 0.7f;  // digital full scale -> volts at the input of the stage
            constexpr float knee = 0.55f;    // the diodes start to clip at about half a volt
            const float x = in * inScale;
            const float v = stage.process (x);           // voltage that wants to appear across the feedback network
            // soft clip with first-order ADAA: average of tanh over [vPrev, v]
            const float z = v / knee, zp = vPrev / knee;
            float c;
            const float dz = z - zp;
            if (std::abs (dz) < 1.0e-4f)
                c = std::tanh (0.5f * (z + zp));
            else
                c = (lnCosh (z) - lnCosh (zp)) / dz;
            vPrev = v;
            const float y = x + knee * c;
            toneState = toneA * y + toneK * toneState;
            // remove DC (asymmetries, denormals)
            dc += (toneState - dc) * 0.0008f;
            return (toneState - dc) / (inScale * 1.4f) * comp;
        }

        void reset() { stage.reset(); vPrev = toneState = dc = 0.0f; }

    private:
        static float lnCosh (float x)
        {
            const float ax = std::abs (x);
            return ax + std::log1p (std::exp (-2.0f * ax)) - 0.6931472f;
        }

        Biquad stage;
        float vPrev = 0.0f, toneState = 0.0f, dc = 0.0f, toneA = 0.5f, toneK = 0.5f, comp = 0.3f;
    };
}
