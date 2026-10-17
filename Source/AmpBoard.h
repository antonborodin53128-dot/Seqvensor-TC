#pragma once

// The signal path of the built-in "Amp": a pedal board in front of the amplifier and the time effects either in front of
// it or in its effects loop (between the pre-amp and the power amp). Plain C++, no JUCE, zero latency.
//
//   guitar -> Drive (808) -> Comp -> EQ -> [Chorus -> Delay -> Reverb]  -> pre-amp + tone stack -> power amp -> cabinet
//                                          (front of the amp)         or  ^ effects loop
//
// Every pedal has a switch that cross-fades (no clicks); a pedal that is off costs nothing.

#include "AmpCore.h"
#include "FxCores.h"
#include "OverdriveCore.h"
#include "ReverbCore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace tcboard
{
    struct Pedals
    {
        bool driveOn = false, compOn = false, eqOn = false, chorusOn = false, delayOn = false, reverbOn = false;
        bool loop = false;                                    // time effects in the effects loop instead of the front

        float drive = 0.5f, driveTone = 0.5f, driveLevelDb = 0.0f;           // 0..1, 0..1, dB
        float compSens = 0.5f, compLevelDb = 0.0f;                           // 0..1, dB
        float eqDb[8] = {};                                                  // dB per band
        float chorusRate = 0.8f, chorusDepth = 0.5f, chorusMix = 0.5f;       // Hz, 0..1, 0..1
        int delayDiv = 5;                                                    // index in tcfx::divisions()
        float delayFeedback = 0.35f, delayMix = 0.3f, delayTone = 0.6f;      // 0..0.95, 0..1, 0..1
        int reverbType = 0;                                                  // 0 Hall, 1 Plate, 2 Room
        float reverbDecay = 2.2f, reverbMix = 0.25f, reverbTone = 0.5f;      // s, 0..1, 0..1
    };

    // linear fade of a switch
    struct Fader
    {
        float a = 0.0f, step = 0.01f;
        bool target = false;
        void prepare (double sr) { step = (float) (1.0 / (0.012 * sr)); }
        bool idle() const { return ! target && a <= 0.0f; }
        float next()
        {
            a += target ? step : -step;
            a = std::max (0.0f, std::min (1.0f, a));
            return a;
        }
    };

    class Board
    {
    public:
        void prepare (double sampleRate)
        {
            sr = sampleRate;
            for (auto& c : amp)
                c.prepare (sr);
            for (auto& d : drive)
                d.reset();
            comp.prepare (sr);
            eq.prepare (sr);
            chorus.prepare (sr);
            delay.prepare (sr);
            reverb.prepare (sr);
            delayLive = reverbLive = false;
            fxStep = (float) (1.0 / (0.050 * sr));
            mixCoef = (float) (1.0 - std::exp (-1.0 / (0.030 * sr)));
            for (auto* f : { &fDrive, &fComp, &fEq, &fChorus, &fDelay, &fReverb })
            {
                f->prepare (sr);
                f->a = 0.0f;
            }
            fDelay.step = fReverb.step = (float) (1.0 / (0.080 * sr));   // delay / reverb leave slowly: the tail fades instead of being cut
            haveAmp = false;
        }

        void resetAmp()
        {
            for (auto& c : amp)
                c.reset();
        }

        // rate: block rate parameters of the amp (changed only when something changed)
        void setAmp (const tcamp::Params& p)
        {
            if (! haveAmp || std::memcmp (&p, &lastAmp, sizeof (p)) != 0)
            {
                lastAmp = p;
                haveAmp = true;
                for (auto& c : amp)
                    c.setParams (p);
            }
        }

        // mono: r == nullptr (the time effects then work on a doubled signal and the left channel gets their mid)
        void process (float* l, float* r, int n, const Pedals& pd, double bpm)
        {
            const bool mono = r == nullptr;
            const int nch = mono ? 1 : 2;
            if (mono)
            {
                scratch.resize ((size_t) n);
                r = scratch.data();
            }

            fDrive.target = pd.driveOn; fComp.target = pd.compOn; fEq.target = pd.eqOn;
            fChorus.target = pd.chorusOn; fDelay.target = pd.delayOn; fReverb.target = pd.reverbOn;

            // ---- pedals in front of the amp
            if (! fDrive.idle())
            {
                for (auto& d : drive)
                {
                    d.setDrive (sr, pd.drive);
                    d.setTone (sr, pd.driveTone);
                }
                const float g = std::pow (10.0f, pd.driveLevelDb / 20.0f);
                float* ch[2] = { l, r };
                for (int c = 0; c < nch; ++c)
                {
                    Fader f = fDrive;
                    for (int i = 0; i < n; ++i)
                    {
                        const float a = f.next();
                        ch[c][i] += a * (drive[c].process (ch[c][i]) * g - ch[c][i]);
                    }
                    if (c == nch - 1)
                        fDrive = f;
                }
            }
            else
                for (auto& d : drive)
                    d.reset();

            if (! fComp.idle())
            {
                comp.setParams (pd.compSens, pd.compLevelDb);
                for (int i = 0; i < n; ++i)
                {
                    const float a = fComp.next();
                    float cl = l[i], cr = mono ? l[i] : r[i];
                    comp.process (cl, cr);
                    l[i] += a * (cl - l[i]);
                    if (! mono)
                        r[i] += a * (cr - r[i]);
                }
            }

            if (! fEq.idle())
            {
                eq.setGains (pd.eqDb);
                Fader f = fEq;
                for (int i = 0; i < n; ++i)
                {
                    const float a = f.next();
                    l[i] += a * (eq.process (0, l[i]) - l[i]);
                }
                if (! mono)
                {
                    Fader f2 = fEq;
                    for (int i = 0; i < n; ++i)
                    {
                        const float a = f2.next();
                        r[i] += a * (eq.process (1, r[i]) - r[i]);
                    }
                }
                fEq = f;
            }

            if (mono)
                std::copy (l, l + n, r);

            // ---- time effects: in front of the amp, or later in its loop
            if (! pd.loop)
            {
                timeEffects (l, r, n, pd, bpm);
                if (mono)
                    for (int i = 0; i < n; ++i)
                        l[i] = 0.5f * (l[i] + r[i]);
            }

            // ---- pre-amp
            for (int i = 0; i < n; ++i)
                l[i] = amp[0].preamp (l[i]);
            if (! mono)
                for (int i = 0; i < n; ++i)
                    r[i] = amp[1].preamp (r[i]);
            else
                std::copy (l, l + n, r);

            if (pd.loop)
            {
                timeEffects (l, r, n, pd, bpm);
                if (mono)
                    for (int i = 0; i < n; ++i)
                        l[i] = 0.5f * (l[i] + r[i]);
            }

            // ---- power amp, cabinet
            for (int i = 0; i < n; ++i)
                l[i] = amp[0].power (l[i]);
            if (! mono)
                for (int i = 0; i < n; ++i)
                    r[i] = amp[1].power (r[i]);
        }

    private:
        void timeEffects (float* l, float* r, int n, const Pedals& pd, double bpm)
        {
            if (! fChorus.idle())
            {
                chorus.setParams (pd.chorusRate, pd.chorusDepth, pd.chorusMix);
                for (int i = 0; i < n; ++i)
                {
                    const float a = fChorus.next();
                    float cl = l[i], cr = r[i];
                    chorus.process (cl, cr);
                    l[i] += a * (cl - l[i]);
                    r[i] += a * (cr - r[i]);
                }
            }

            // Delay and reverb sit behind their own "post-fader": when the preset changes the delay time / reverb type, the wet
            // signal is ducked smoothly, the engine is switched while it is silent, and the wet signal comes back. The mix is
            // smoothed too. One engine each, so the load does not change.
            if (! fDelay.idle())
            {
                if (! delayLive)
                {
                    delayLive = true;
                    curDiv = pd.delayDiv;
                    gDelay = 1.0f;
                    mixDelay = pd.delayMix;
                    duckDelay = false;
                }
                tcfx::DelayEngine::Params dp;
                dp.feedback = std::max (0.0f, std::min (0.95f, pd.delayFeedback));
                dp.mode = tcfx::Stereo;
                dp.lpHz = 1500.0f * std::pow (8.0f, std::max (0.0f, std::min (1.0f, pd.delayTone)));
                dp.hpHz = 120.0f;
                dp.timeL = dp.timeR = tcfx::divisionSeconds (curDiv, bpm);
                delay.setParams (dp);
                if (pd.delayDiv != curDiv)
                    duckDelay = true;
                for (int i = 0; i < n; ++i)
                {
                    if (duckDelay)
                    {
                        gDelay -= fxStep;
                        if (gDelay <= 0.0f)
                        {
                            gDelay = 0.0f;
                            duckDelay = false;
                            curDiv = pd.delayDiv;
                            delay.reset();
                            dp.timeL = dp.timeR = tcfx::divisionSeconds (curDiv, bpm);
                            delay.setParams (dp);
                        }
                    }
                    else
                        gDelay = std::min (1.0f, gDelay + fxStep);
                    mixDelay += mixCoef * (pd.delayMix - mixDelay);
                    const float a = fDelay.next() * gDelay * mixDelay;
                    float wl, wr;
                    delay.process (l[i], r[i], wl, wr);
                    l[i] += a * wl;
                    r[i] += a * wr;
                }
            }
            else if (delayLive)
            {
                delay.reset();
                delayLive = false;
            }

            if (! fReverb.idle())
            {
                if (! reverbLive)
                {
                    reverbLive = true;
                    curRev = pd.reverbType;
                    gReverb = 1.0f;
                    mixReverb = pd.reverbMix;
                    duckReverb = false;
                }
                reverb.setParams (curRev, 0.0f, std::max (0.3f, pd.reverbDecay), std::max (0.0f, std::min (1.0f, pd.reverbTone)));
                if (pd.reverbType != curRev)
                    duckReverb = true;
                for (int i = 0; i < n; ++i)
                {
                    if (duckReverb)
                    {
                        gReverb -= fxStep;
                        if (gReverb <= 0.0f)
                        {
                            gReverb = 0.0f;
                            duckReverb = false;
                            curRev = pd.reverbType;
                            reverb.reset();
                            reverb.setParams (curRev, 0.0f, std::max (0.3f, pd.reverbDecay), std::max (0.0f, std::min (1.0f, pd.reverbTone)));
                        }
                    }
                    else
                        gReverb = std::min (1.0f, gReverb + fxStep);
                    mixReverb += mixCoef * (pd.reverbMix - mixReverb);
                    const float a = fReverb.next() * gReverb * mixReverb;
                    float wl, wr;
                    reverb.process (l[i], r[i], wl, wr);
                    l[i] += a * wl;
                    r[i] += a * wr;
                }
            }
            else if (reverbLive)
            {
                reverb.reset();
                reverbLive = false;
            }
        }

        double sr = 48000.0;
        tcamp::Channel amp[2];
        tcod::Channel drive[2];
        tcfx::Compressor comp;
        tcfx::Eq8 eq;
        tcfx::Chorus chorus;
        tcfx::DelayEngine delay;
        tcrev::Core reverb;
        Fader fDrive, fComp, fEq, fChorus, fDelay, fReverb;
        tcamp::Params lastAmp;
        bool haveAmp = false, delayLive = false, reverbLive = false, duckDelay = false, duckReverb = false;
        int curDiv = 5, curRev = 0;
        float gDelay = 1.0f, gReverb = 1.0f, mixDelay = 0.0f, mixReverb = 0.0f, fxStep = 0.001f, mixCoef = 0.001f;
        std::vector<float> scratch;
    };
}
