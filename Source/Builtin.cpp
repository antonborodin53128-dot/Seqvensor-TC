#include "Builtin.h"
#include "ReverbCore.h"
#include "OverdriveCore.h"
#include "AmpCore.h"
#include "AmpBoard.h"
#include "AutotuneCore.h"
#include "Text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    constexpr float pi = 3.14159265358979f;

    // RBJ biquad, transposed direct form II, one state per channel
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1[2] = {}, z2[2] = {};

        void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0f; }

        void set (float nb0, float nb1, float nb2, float na0, float na1, float na2)
        {
            b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0;
            a1 = na1 / na0; a2 = na2 / na0;
        }

        void makePeak (double sr, float f, float gainDb, float q)
        {
            const float A = std::pow (10.0f, gainDb / 40.0f);
            const float w = 2.0f * pi * f / (float) sr, c = std::cos (w), al = std::sin (w) / (2.0f * q);
            set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
        }

        void makeLowShelf (double sr, float f, float gainDb, float s = 0.8f)
        {
            const float A = std::pow (10.0f, gainDb / 40.0f);
            const float w = 2.0f * pi * f / (float) sr, c = std::cos (w), sn = std::sin (w);
            const float al = sn / 2.0f * std::sqrt ((A + 1 / A) * (1 / s - 1) + 2);
            const float sq = 2.0f * std::sqrt (A) * al;
            set (A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq),
                 (A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq);
        }

        void makeHighShelf (double sr, float f, float gainDb, float s = 0.8f)
        {
            const float A = std::pow (10.0f, gainDb / 40.0f);
            const float w = 2.0f * pi * f / (float) sr, c = std::cos (w), sn = std::sin (w);
            const float al = sn / 2.0f * std::sqrt ((A + 1 / A) * (1 / s - 1) + 2);
            const float sq = 2.0f * std::sqrt (A) * al;
            set (A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq),
                 (A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq);
        }

        void makeHighPass (double sr, float f, float q = 0.7071f)
        {
            const float w = 2.0f * pi * f / (float) sr, c = std::cos (w), al = std::sin (w) / (2.0f * q);
            set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
        }

        void makeLowPass (double sr, float f, float q = 0.7071f)
        {
            const float w = 2.0f * pi * f / (float) sr, c = std::cos (w), al = std::sin (w) / (2.0f * q);
            set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
        }

        float process (int ch, float x)
        {
            const float y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            return y;
        }
    };

    // ------------------------------------------------------------ common part of all built-in effects
    class BuiltinBase : public juce::AudioPluginInstance
    {
    public:
        BuiltinBase (juce::String idText, juce::String nameText, juce::String descriptionText)
            : juce::AudioPluginInstance (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                                         .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
              id (std::move (idText)), name (std::move (nameText)), description (std::move (descriptionText))
        {
        }

        void fillInPluginDescription (juce::PluginDescription& d) const override
        {
            d.name = name;
            d.descriptiveName = description;
            d.pluginFormatName = BuiltinFormat::formatName();
            d.category = "Fx";
            d.manufacturerName = "TC";
            d.version = "1.0";
            d.fileOrIdentifier = "TC:" + id;
            d.uniqueId = d.deprecatedUid = id.hashCode();
            d.isInstrument = false;
            d.numInputChannels = 2;
            d.numOutputChannels = 2;
        }

        const juce::String getName() const override { return name; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        double getTailLengthSeconds() const override { return 0.0; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        bool hasEditor() const override { return true; }
        juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }

        bool isBusesLayoutSupported (const BusesLayout& l) const override
        {
            const auto& in = l.getMainInputChannelSet();
            return (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono()) && in == l.getMainOutputChannelSet();
        }

        void prepareToPlay (double sampleRate, int) override
        {
            sr = sampleRate;
            prepare();
        }
        void releaseResources() override {}

        void getStateInformation (juce::MemoryBlock& dest) override
        {
            juce::XmlElement xml ("TCFX");
            for (auto* p : getParameters())
                if (auto* pp = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                    xml.setAttribute (pp->paramID, (double) pp->getValue());
            copyXmlToBinary (xml, dest);
        }

        void setStateInformation (const void* data, int size) override
        {
            if (auto xml = getXmlFromBinary (data, size))
                for (auto* p : getParameters())
                    if (auto* pp = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                        if (xml->hasAttribute (pp->paramID))
                            pp->setValueNotifyingHost ((float) xml->getDoubleAttribute (pp->paramID));
        }

    protected:
        virtual void prepare() {}

        juce::AudioParameterFloat* addFloat (const char* pid, const juce::String& label, float lo, float hi, float def, float skew = 1.0f)
        {
            juce::NormalisableRange<float> range (lo, hi);
            range.setSkewForCentre (skew > 0.0f ? lo + (hi - lo) * skew : (lo + hi) * 0.5f);
            auto* p = new juce::AudioParameterFloat (juce::ParameterID { pid, 1 }, label, range, def);
            juce::AudioProcessor::addParameter (p);
            return p;
        }

        // linear parameter
        juce::AudioParameterFloat* addLinear (const char* pid, const juce::String& label, float lo, float hi, float def)
        {
            auto* p = new juce::AudioParameterFloat (juce::ParameterID { pid, 1 }, label, juce::NormalisableRange<float> (lo, hi), def);
            juce::AudioProcessor::addParameter (p);
            return p;
        }

        juce::String id, name, description;
        double sr = 48000.0;
    };

    // ------------------------------------------------------------ equaliser in the style of a graphic "pro" EQ
    // Up to kBands bands, each: Bell / Low Shelf / High Shelf / Low Cut / High Cut / Notch.
    // Zero latency: minimum-phase IIR sections only (no look-ahead, no FFT in the audio path). The spectrum analyser
    // only copies samples into a lock-free FIFO; all of its work happens in the editor.
    constexpr int kBands = 10;
    constexpr float kMinFreq = 10.0f, kMaxFreq = 30000.0f;
    const char* const kTypeNames[] = { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch" };
    const char* const kSlopeNames[] = { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" };

    struct Coef { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    inline Coef normCoef (float b0, float b1, float b2, float a0, float a1, float a2)
    {
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    // Bell whose magnitude follows the analogue prototype up to Nyquist (matched at DC, the centre and Nyquist, after M. Vicanek).
    // The usual bilinear bell is pulled to 0 dB at Nyquist, so a band near 20 kHz showed (and did) a sudden fall; this one keeps going.
    inline bool matchedBell (double sr, float fIn, float gDb, float qIn, Coef& out)
    {
        const double pi_ = 3.14159265358979323846, f = fIn, Q = qIn;
        const double A = std::pow (10.0, gDb / 40.0), G = A * A, w0 = 2.0 * pi_ * f / sr;
        if (std::abs (gDb) < 0.01 || w0 < 0.01)
            return false;
        const double zeta = 1.0 / (2.0 * A * Q);
        const double a1 = zeta < 1.0 ? -2.0 * std::exp (-zeta * w0) * std::cos (std::sqrt (1.0 - zeta * zeta) * w0)
                                     : -2.0 * std::exp (-zeta * w0) * std::cosh (std::sqrt (zeta * zeta - 1.0) * w0);
        const double a2 = std::exp (-2.0 * zeta * w0);
        const double A0 = (1 + a1 + a2) * (1 + a1 + a2), A1 = (1 - a1 + a2) * (1 - a1 + a2), A2 = -4.0 * a2;
        const double p1 = std::sin (w0 / 2) * std::sin (w0 / 2), p0 = 1.0 - p1, p2 = 4.0 * p0 * p1;
        if (p2 < 1.0e-9)
            return false;
        const double om = pi_ / w0, u = (1 - om * om) * (1 - om * om);
        const double hn = (u + (A * om / Q) * (A * om / Q)) / (u + (om / (A * Q)) * (om / (A * Q)));
        const double B0 = A0, B1 = A1 * hn;
        const double B2 = (G * G * (A0 * p0 + A1 * p1 + A2 * p2) - B0 * p0 - B1 * p1) / p2;
        const double s0 = std::sqrt (B0), s1 = std::sqrt (juce::jmax (0.0, B1)), W = 0.5 * (s0 + s1), d = W * W + B2;
        if (d < 0.0 || ! std::isfinite (d) || a2 >= 1.0)
            return false;
        const double b0 = 0.5 * (W + std::sqrt (d)), b2 = 0.5 * (W - std::sqrt (d)), b1 = 0.5 * (s0 - s1);
        out = normCoef ((float) b0, (float) b1, (float) b2, 1.0f, (float) a1, (float) a2);
        return true;
    }

    // returns the number of 2nd-order sections (1..4)
    int designBand (int type, double sr, float f, float gDb, float q, int slope, Coef* out)
    {
        f = juce::jlimit (5.0f, (float) sr * 0.49f, f);
        q = juce::jmax (0.05f, q);
        const float w = 2.0f * pi * f / (float) sr, c = std::cos (w), s = std::sin (w);

        switch (type)
        {
            case 0:   // bell
            {
                const float A = std::pow (10.0f, gDb / 40.0f), al = s / (2.0f * q);
                if (matchedBell (sr, f, gDb, q, out[0]))
                    return 1;
                out[0] = normCoef (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
                return 1;
            }
            case 1:   // low shelf
            case 2:   // high shelf
            {
                const float A = std::pow (10.0f, gDb / 40.0f), al = s / (2.0f * q), sq = 2.0f * std::sqrt (A) * al;
                if (type == 1)
                    out[0] = normCoef (A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq),
                                       (A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq);
                else
                    out[0] = normCoef (A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq),
                                       (A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq);
                return 1;
            }
            case 5:   // notch
            {
                const float al = s / (2.0f * q);
                out[0] = normCoef (1, -2 * c, 1, 1 + al, -2 * c, 1 - al);
                return 1;
            }
            default:  // 3 = low cut (high-pass), 4 = high cut (low-pass): Butterworth, 12..48 dB/oct
            {
                const int sections = juce::jlimit (1, 4, slope + 1);
                const int order = sections * 2;
                for (int i = 0; i < sections; ++i)
                {
                    const float qi = 1.0f / (2.0f * std::cos (pi * (float) (2 * i + 1) / (float) (2 * order)));
                    const float al = s / (2.0f * qi);
                    if (type == 3)
                        out[i] = normCoef ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
                    else
                        out[i] = normCoef ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
                }
                return sections;
            }
        }
    }

    // magnitude of one section in dB at angular frequency w
    inline float sectionDb (const Coef& k, float w)
    {
        const float c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2 * w), s2 = std::sin (2 * w);
        const float nr = k.b0 + k.b1 * c1 + k.b2 * c2, ni = -(k.b1 * s1 + k.b2 * s2);
        const float dr = 1 + k.a1 * c1 + k.a2 * c2, di = -(k.a1 * s1 + k.a2 * s2);
        const float num = nr * nr + ni * ni, den = dr * dr + di * di;
        return 10.0f * std::log10 (juce::jmax (1.0e-20f, num) / juce::jmax (1.0e-20f, den));
    }

    // lock-free single-producer / single-consumer copy of the audio for the analyser
    struct SampleFifo
    {
        static constexpr int size = 16384;
        juce::AbstractFifo fifo { size };
        std::vector<float> data = std::vector<float> ((size_t) size, 0.0f);

        void push (const juce::AudioBuffer<float>& b)
        {
            const int channels = b.getNumChannels();
            const int total = b.getNumSamples();
            if (channels == 0)
                return;
            float chunk[512];
            for (int pos = 0; pos < total; pos += 512)
            {
                const int n = juce::jmin (512, total - pos);
                for (int i = 0; i < n; ++i)
                {
                    float sum = 0.0f;
                    for (int c = 0; c < channels; ++c)
                        sum += b.getReadPointer (c)[pos + i];
                    chunk[i] = sum / (float) channels;
                }
                int s1, n1, s2, n2;
                fifo.prepareToWrite (n, s1, n1, s2, n2);
                if (n1 > 0) std::copy (chunk, chunk + n1, data.begin() + s1);
                if (n2 > 0) std::copy (chunk + n1, chunk + n1 + n2, data.begin() + s2);
                fifo.finishedWrite (n1 + n2);
            }
        }

        int pull (float* dest, int maxCount)
        {
            const int n = juce::jmin (maxCount, fifo.getNumReady());
            int s1, n1, s2, n2;
            fifo.prepareToRead (n, s1, n1, s2, n2);
            if (n1 > 0) std::copy (data.begin() + s1, data.begin() + s1 + n1, dest);
            if (n2 > 0) std::copy (data.begin() + s2, data.begin() + s2 + n2, dest + n1);
            fifo.finishedRead (n1 + n2);
            return n1 + n2;
        }
    };

    class EqEditor;

    class EqPlugin : public BuiltinBase
    {
    public:
        struct Band
        {
            juce::AudioParameterBool* on = nullptr;
            juce::AudioParameterChoice* type = nullptr;
            juce::AudioParameterFloat *freq = nullptr, *gain = nullptr, *q = nullptr;
            juce::AudioParameterChoice* slope = nullptr;
        };

        EqPlugin() : BuiltinBase ("eq", "Equalizer", "Parametric equalizer with spectrum analyzer")
        {
            for (int i = 0; i < kBands; ++i)
            {
                const juce::String n = "Band " + juce::String (i + 1) + " ";
                const juce::String id = "b" + juce::String (i);
                auto& b = bands[(size_t) i];
                b.on = new juce::AudioParameterBool (juce::ParameterID { id + "on", 1 }, n + "On", false);
                b.type = new juce::AudioParameterChoice (juce::ParameterID { id + "ty", 1 }, n + "Type",
                                                         juce::StringArray { kTypeNames[0], kTypeNames[1], kTypeNames[2], kTypeNames[3], kTypeNames[4], kTypeNames[5] }, 0);
                juce::NormalisableRange<float> fr (kMinFreq, kMaxFreq);
                fr.setSkewForCentre (1000.0f);
                b.freq = new juce::AudioParameterFloat (juce::ParameterID { id + "fr", 1 }, n + "Frequency", fr, 1000.0f,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                    {
                        return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (v, v < 100.0f ? 1 : 0) + " Hz";
                    }));
                b.gain = new juce::AudioParameterFloat (juce::ParameterID { id + "ga", 1 }, n + "Gain", juce::NormalisableRange<float> (-30.0f, 30.0f), 0.0f,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                    {
                        return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB";
                    }));
                juce::NormalisableRange<float> qr (0.1f, 40.0f);
                qr.setSkewForCentre (1.0f);
                b.q = new juce::AudioParameterFloat (juce::ParameterID { id + "q", 1 }, n + "Q", qr, 1.0f,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2); }));
                b.slope = new juce::AudioParameterChoice (juce::ParameterID { id + "sl", 1 }, n + "Slope",
                                                          juce::StringArray { kSlopeNames[0], kSlopeNames[1], kSlopeNames[2], kSlopeNames[3] }, 0);
                for (juce::RangedAudioParameter* p : { (juce::RangedAudioParameter*) b.on, (juce::RangedAudioParameter*) b.type,
                                                       (juce::RangedAudioParameter*) b.freq, (juce::RangedAudioParameter*) b.gain,
                                                       (juce::RangedAudioParameter*) b.q, (juce::RangedAudioParameter*) b.slope })
                    juce::AudioProcessor::addParameter (p);
            }
            out = new juce::AudioParameterFloat (juce::ParameterID { "out", 1 }, "Output Gain", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f,
                juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                {
                    return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB";
                }));
            juce::AudioProcessor::addParameter (out);
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            const int channels = juce::jmin (2, buffer.getNumChannels());
            const int n = buffer.getNumSamples();

            pre.push (buffer);

            for (int i = 0; i < kBands; ++i)
            {
                auto& b = bands[(size_t) i];
                auto& d = dsp[(size_t) i];
                if (! b.on->get())
                {
                    d.sections = 0;
                    continue;
                }

                const int type = b.type->getIndex(), slope = b.slope->getIndex();
                const float f = b.freq->get(), g = b.gain->get(), q = b.q->get();
                if (d.sections == 0 || type != d.type || slope != d.slope || f != d.freq || g != d.gain || q != d.q)
                {
                    const int before = d.sections;
                    Coef c[4];
                    const int count = designBand (type, sr, f, g, q, slope, c);
                    for (int k = 0; k < count; ++k)
                        d.coef[k] = c[k];
                    if (before == 0 || count != before)
                        std::memset (d.z, 0, sizeof (d.z));
                    d.sections = count;
                    d.type = type; d.slope = slope; d.freq = f; d.gain = g; d.q = q;
                }
            }

            for (int i = 0; i < kBands; ++i)
            {
                auto& d = dsp[(size_t) i];
                for (int k = 0; k < d.sections; ++k)
                {
                    const Coef& c = d.coef[k];
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        float* x = buffer.getWritePointer (ch);
                        float z1 = d.z[k][ch][0], z2 = d.z[k][ch][1];
                        for (int s = 0; s < n; ++s)
                        {
                            const float in = x[s];
                            const float y = c.b0 * in + z1;
                            z1 = c.b1 * in - c.a1 * y + z2;
                            z2 = c.b2 * in - c.a2 * y;
                            x[s] = y;
                        }
                        d.z[k][ch][0] = z1;
                        d.z[k][ch][1] = z2;
                    }
                }
            }

            const float gain = std::pow (10.0f, out->get() / 20.0f);
            if (gain != 1.0f)
                buffer.applyGain (gain);

            post.push (buffer);
        }

        juce::AudioProcessorEditor* createEditor() override;
        bool hasEditor() const override { return true; }

        std::array<Band, kBands> bands;
        juce::AudioParameterFloat* out = nullptr;
        SampleFifo pre, post;
        std::atomic<int> analyzerMode { 0 };   // 0 = Pre + Post, 1 = Pre, 2 = Post, 3 = off
        std::atomic<int> rangeIndex { 1 };     // 6 / 12 / 18 / 30 dB

        double getRate() const { return sr; }

    protected:
        void prepare() override
        {
            for (auto& d : dsp)
            {
                d.sections = 0;
                std::memset (d.z, 0, sizeof (d.z));
            }
        }

    private:
        struct Dsp
        {
            Coef coef[4];
            float z[4][2][2] = {};
            int sections = 0, type = -1, slope = -1;
            float freq = 0, gain = 0, q = 0;
        };
        std::array<Dsp, kBands> dsp;
    };

    // small radix-2 FFT for the analyser (editor thread only)
    struct Fft
    {
        static constexpr int N = 4096;
        std::vector<float> re = std::vector<float> ((size_t) N), im = std::vector<float> ((size_t) N), window = std::vector<float> ((size_t) N);
        Fft()
        {
            for (int i = 0; i < N; ++i)
                window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) N);
        }
        // in: N samples; out: N/2 magnitudes in dBFS
        void run (const float* in, float* outDb)
        {
            for (int i = 0; i < N; ++i)
            {
                re[(size_t) i] = in[i] * window[(size_t) i];
                im[(size_t) i] = 0.0f;
            }
            for (int i = 1, j = 0; i < N; ++i)
            {
                int bit = N >> 1;
                for (; j & bit; bit >>= 1)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                {
                    std::swap (re[(size_t) i], re[(size_t) j]);
                    std::swap (im[(size_t) i], im[(size_t) j]);
                }
            }
            for (int len = 2; len <= N; len <<= 1)
            {
                const float ang = -2.0f * pi / (float) len;
                const float wr = std::cos (ang), wi = std::sin (ang);
                for (int i = 0; i < N; i += len)
                {
                    float cr = 1.0f, ci = 0.0f;
                    for (int k = 0; k < len / 2; ++k)
                    {
                        const size_t a = (size_t) (i + k), b = (size_t) (i + k + len / 2);
                        const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                        re[b] = re[a] - tr; im[b] = im[a] - ti;
                        re[a] += tr; im[a] += ti;
                        const float ncr = cr * wr - ci * wi;
                        ci = cr * wi + ci * wr;
                        cr = ncr;
                    }
                }
            }
            for (int i = 0; i < N / 2; ++i)
            {
                const float mag = std::sqrt (re[(size_t) i] * re[(size_t) i] + im[(size_t) i] * im[(size_t) i]) / ((float) N / 4.0f);
                outDb[i] = 20.0f * std::log10 (juce::jmax (1.0e-7f, mag));
            }
        }
    };

    // ------------------------------------------------------------ the editor
    class EqEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit EqEditor (EqPlugin& p) : juce::AudioProcessorEditor (p), eq (p)
        {
            setSize (900, 560);
            setResizable (true, true);
            setResizeLimits (720, 460, 1800, 1100);

            for (auto* b : { &analyzerBtn, &rangeBtn, &delBtn })
            {
                b->setColour (juce::TextButton::buttonColourId, bg2);
                b->setColour (juce::TextButton::textColourOffId, text);
                addAndMakeVisible (*b);
            }
            analyzerBtn.onClick = [this]
            {
                eq.analyzerMode = (eq.analyzerMode.load() + 1) % 4;
                refreshButtons();
            };
            rangeBtn.onClick = [this]
            {
                eq.rangeIndex = (eq.rangeIndex.load() + 1) % 4;
                refreshButtons();
            };
            delBtn.setButtonText ("X");
            delBtn.onClick = [this] { removeBand (selected); };

            onBtn.setButtonText ("");
            onBtn.setColour (juce::ToggleButton::tickColourId, accent);
            addAndMakeVisible (onBtn);
            typeBox.addItemList (juce::StringArray { kTypeNames[0], kTypeNames[1], kTypeNames[2], kTypeNames[3], kTypeNames[4], kTypeNames[5] }, 1);
            slopeBox.addItemList (juce::StringArray { kSlopeNames[0], kSlopeNames[1], kSlopeNames[2], kSlopeNames[3] }, 1);
            for (auto* c : { &typeBox, &slopeBox })
            {
                c->setColour (juce::ComboBox::backgroundColourId, bg2);
                c->setColour (juce::ComboBox::textColourId, text);
                c->setColour (juce::ComboBox::outlineColourId, border);
                addAndMakeVisible (*c);
            }
            for (auto* s : { &freqS, &gainS, &qS })
            {
                s->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
                s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 18);
                s->setColour (juce::Slider::rotarySliderFillColourId, accent);
                s->setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3a4152));
                s->setColour (juce::Slider::thumbColourId, text);
                s->setColour (juce::Slider::textBoxTextColourId, accent);
                s->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
                addAndMakeVisible (*s);
            }
            outS.setSliderStyle (juce::Slider::LinearHorizontal);
            outS.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 18);
            outS.setColour (juce::Slider::trackColourId, accent);
            outS.setColour (juce::Slider::thumbColourId, text);
            outS.setColour (juce::Slider::textBoxTextColourId, text);
            outS.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            addAndMakeVisible (outS);
            outAtt = std::make_unique<juce::SliderParameterAttachment> (*eq.out, outS);

            refreshButtons();
            select (-1);
            startTimerHz (30);
        }

        ~EqEditor() override { stopTimer(); }

        void resized() override
        {
            auto r = getLocalBounds();
            header = r.removeFromTop (38);
            footer = r.removeFromBottom (28);
            graph = r.reduced (46, 8).withTrimmedRight (30).toFloat();

            analyzerBtn.setBounds (header.getRight() - 700, 6, 160, 26);
            rangeBtn.setBounds (header.getRight() - 530, 6, 80, 26);
            outS.setBounds (header.getRight() - 360, 6, 350, 26);

            const int pw = 620, ph = 138;
            panel = juce::Rectangle<int> (graph.getCentreX() - pw / 2, (int) graph.getBottom() - ph - 26, pw, ph).toNearestInt();
            onBtn.setBounds (panel.getX() + 10, panel.getY() + 10, 28, 28);
            typeBox.setBounds (panel.getX() + 44, panel.getY() + 10, 120, 28);
            slopeBox.setBounds (panel.getX() + 44, panel.getY() + 48, 120, 28);
            freqS.setBounds (panel.getX() + 190, panel.getY() + 24, 104, 108);
            gainS.setBounds (panel.getX() + 304, panel.getY() + 24, 104, 108);
            qS.setBounds (panel.getX() + 418, panel.getY() + 24, 104, 108);
            delBtn.setBounds (panel.getRight() - 40, panel.getY() + 10, 30, 28);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (bg);

            g.setColour (accent);
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText ("TC  EQUALIZER", header.withTrimmedLeft (16), juce::Justification::centredLeft);
            g.setColour (muted);
            g.setFont (12.0f);
            g.drawText ("Output", juce::Rectangle<int> (outS.getX() - 60, outS.getY(), 54, outS.getHeight()), juce::Justification::centredRight);

            auto gr = graph;
            g.setColour (juce::Colour (0xff0d0f13));
            g.fillRoundedRectangle (gr, 6.0f);

            const float range = currentRange();

            // grid
            g.setFont (11.0f);
            for (float f : { 20.f, 30.f, 40.f, 50.f, 60.f, 70.f, 80.f, 90.f, 100.f, 200.f, 300.f, 400.f, 500.f, 600.f, 700.f, 800.f, 900.f,
                             1000.f, 2000.f, 3000.f, 4000.f, 5000.f, 6000.f, 7000.f, 8000.f, 9000.f, 10000.f, 20000.f })
            {
                const float x = freqToX (f);
                const bool major = f == 20 || f == 50 || f == 100 || f == 200 || f == 500 || f == 1000 || f == 2000 || f == 5000 || f == 10000 || f == 20000;
                g.setColour (juce::Colour (major ? 0xff262c38 : 0xff1a1f28));
                g.drawVerticalLine ((int) x, gr.getY(), gr.getBottom());
                if (major)
                {
                    g.setColour (muted);
                    g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f),
                                juce::Rectangle<float> (x - 20, gr.getBottom() - 16, 40, 14).toNearestInt(), juce::Justification::centred);
                }
            }
            for (int i = -4; i <= 4; ++i)
            {
                const float db = range * (float) i / 4.0f;
                const float y = dbToY (db);
                g.setColour (juce::Colour (i == 0 ? 0xff3a4152 : 0xff1f2430));
                g.drawHorizontalLine ((int) y, gr.getX(), gr.getRight());
                g.setColour (accent.withAlpha (0.85f));
                g.drawText ((db > 0 ? "+" : "") + juce::String ((int) std::round (db)), juce::Rectangle<float> (gr.getX() - 40, y - 8, 36, 16).toNearestInt(),
                            juce::Justification::centredRight);
            }
            for (int d = 0; d <= 100; d += 20)   // dBFS scale of the analyser on the right
            {
                g.setColour (muted);
                g.drawText (juce::String (-d), juce::Rectangle<float> (gr.getRight() + 2, specToY ((float) -d) - 8, 30, 16).toNearestInt(),
                            juce::Justification::centredLeft);
            }

            g.saveState();
            g.reduceClipRegion (gr.toNearestInt());

            // analyser
            const int mode = eq.analyzerMode.load();
            if (mode == 0 || mode == 1)
                drawSpectrum (g, preDb, accent.withAlpha (0.22f), accent.withAlpha (0.55f), true);
            if (mode == 0 || mode == 2)
                drawSpectrum (g, postDb, juce::Colours::transparentBlack, text.withAlpha (0.5f), false);

            // curves
            const int steps = juce::jmax (2, (int) gr.getWidth() / 2);
            std::vector<float> total ((size_t) steps + 1, 0.0f);
            for (int i = 0; i < kBands; ++i)
                if (eq.bands[(size_t) i].on->get())
                    for (int s = 0; s <= steps; ++s)
                        total[(size_t) s] += bandDb (i, xToFreq (gr.getX() + gr.getWidth() * (float) s / (float) steps));

            if (selected >= 0 && eq.bands[(size_t) selected].on->get())
            {
                juce::Path sp;
                const float y0 = dbToY (0.0f);
                sp.startNewSubPath (gr.getX(), y0);
                for (int s = 0; s <= steps; ++s)
                    sp.lineTo (gr.getX() + gr.getWidth() * (float) s / (float) steps,
                               dbToY (bandDb (selected, xToFreq (gr.getX() + gr.getWidth() * (float) s / (float) steps))));
                sp.lineTo (gr.getRight(), y0);
                sp.closeSubPath();
                g.setColour (accent.withAlpha (0.2f));
                g.fillPath (sp);
            }

            juce::Path curve, fill;
            for (int s = 0; s <= steps; ++s)
            {
                const float x = gr.getX() + gr.getWidth() * (float) s / (float) steps;
                const float y = dbToY (juce::jlimit (-range * 1.2f, range * 1.2f, total[(size_t) s]));
                if (s == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
            }
            fill = curve;
            fill.lineTo (gr.getRight(), dbToY (0.0f));
            fill.lineTo (gr.getX(), dbToY (0.0f));
            fill.closeSubPath();
            g.setColour (accent.withAlpha (0.1f));
            g.fillPath (fill);
            g.setColour (accent);
            g.strokePath (curve, juce::PathStrokeType (2.5f));

            // nodes
            for (int i = 0; i < kBands; ++i)
            {
                if (! eq.bands[(size_t) i].on->get())
                    continue;
                const auto c = nodePos (i);
                const bool sel = i == selected, hov = i == hover;
                if (sel)
                {
                    g.setColour (accent.withAlpha (0.25f));
                    g.fillEllipse (c.x - 15, c.y - 15, 30, 30);
                }
                g.setColour (sel ? juce::Colours::white : (hov ? accent.brighter (0.4f) : accent));
                g.fillEllipse (c.x - 9, c.y - 9, 18, 18);
                g.setColour (bg);
                g.fillEllipse (c.x - 6.5f, c.y - 6.5f, 13, 13);
                g.setColour (sel ? juce::Colours::white : accent);
                g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
                g.drawText (juce::String (i + 1), juce::Rectangle<float> (c.x - 9, c.y - 9, 18, 18).toNearestInt(), juce::Justification::centred);
            }
            g.restoreState();

            g.setColour (border);
            g.drawRoundedRectangle (gr, 6.0f, 1.0f);

            if (selected < 0)
            {
                g.setColour (muted);
                g.setFont (13.0f);
                g.drawText ("Double-click the graph to add a band  -  drag a point  -  mouse wheel = Q  -  right-click = type",
                            gr.toNearestInt().removeFromBottom (60).removeFromTop (30), juce::Justification::centred);
            }
            else
            {
                g.setColour (juce::Colour (0xf0181b22));
                g.fillRoundedRectangle (panel.toFloat(), 8.0f);
                g.setColour (border);
                g.drawRoundedRectangle (panel.toFloat(), 8.0f, 1.0f);
                g.setColour (accent);
                g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                g.drawText ("BAND " + juce::String (selected + 1), juce::Rectangle<int> (panel.getX() + 12, panel.getBottom() - 30, 110, 20),
                            juce::Justification::centredLeft);
                g.setColour (muted);
                g.setFont (11.0f);
                const int labelY = panel.getY() + 6;
                g.drawText ("FREQ", juce::Rectangle<int> (freqS.getX(), labelY, freqS.getWidth(), 14), juce::Justification::centred);
                g.drawText ("GAIN", juce::Rectangle<int> (gainS.getX(), labelY, gainS.getWidth(), 14), juce::Justification::centred);
                g.drawText ("Q", juce::Rectangle<int> (qS.getX(), labelY, qS.getWidth(), 14), juce::Justification::centred);
            }

            g.setColour (muted);
            g.setFont (12.0f);
            g.drawText ("Zero Latency", footer.withTrimmedLeft (16), juce::Justification::centredLeft);
            g.drawText ("Analyzer input: pre-EQ" + juce::String (mode == 0 ? " + post-EQ" : mode == 2 ? " (post only)" : mode == 3 ? " (off)" : ""),
                        footer.withTrimmedRight (16), juce::Justification::centredRight);
        }

        // ---- mouse
        void mouseMove (const juce::MouseEvent& e) override
        {
            const int h = hitNode (e.position);
            if (h != hover)
            {
                hover = h;
                setMouseCursor (h >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
                repaint();
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (panelVisible() && panel.contains (e.getPosition()))
                return;
            const int h = hitNode (e.position);
            if (e.mods.isPopupMenu())
            {
                if (h >= 0)
                {
                    select (h);
                    showTypeMenu (h);
                }
                return;
            }
            if (h >= 0)
            {
                select (h);
                dragging = h;
                bandBegin (h);
            }
            else
            {
                select (-1);
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragging < 0)
                return;
            auto& b = eq.bands[(size_t) dragging];
            setParam (b.freq, juce::jlimit (kMinFreq, kMaxFreq, xToFreq (e.position.x)));
            const int type = b.type->getIndex();
            if (type != 3 && type != 4)
                setParam (b.gain, juce::jlimit (-30.0f, 30.0f, yToDb (e.position.y)));
            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragging >= 0)
                bandEnd (dragging);
            dragging = -1;
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (panelVisible() && panel.contains (e.getPosition()))
                return;
            const int h = hitNode (e.position);
            if (h >= 0)
            {
                removeBand (h);
                return;
            }
            if (! graph.contains (e.position))
                return;
            for (int i = 0; i < kBands; ++i)
                if (! eq.bands[(size_t) i].on->get())
                {
                    auto& b = eq.bands[(size_t) i];
                    bandBegin (i);
                    setParam (b.type, 0.0f);
                    setParam (b.slope, 0.0f);
                    setParam (b.freq, juce::jlimit (kMinFreq, kMaxFreq, xToFreq (e.position.x)));
                    setParam (b.gain, juce::jlimit (-30.0f, 30.0f, yToDb (e.position.y)));
                    setParam (b.q, 1.0f);
                    setParam (b.on, 1.0f);
                    bandEnd (i);
                    select (i);
                    return;
                }
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            int target = hitNode (e.position);
            if (target < 0)
                target = selected;
            if (target < 0 || ! eq.bands[(size_t) target].on->get())
                return;
            auto& b = eq.bands[(size_t) target];
            const float factor = std::pow (1.25f, w.deltaY * 4.0f);
            bandBegin (target);
            setParam (b.q, juce::jlimit (0.1f, 40.0f, b.q->get() * factor));
            bandEnd (target);
            repaint();
        }

    private:
        static void setParam (juce::RangedAudioParameter* p, float plain) { p->setValueNotifyingHost (p->convertTo0to1 (plain)); }
        static void bandBegin (int) {}
        static void bandEnd (int) {}

        bool panelVisible() const { return selected >= 0; }
        float currentRange() const { static const float r[] = { 6.0f, 12.0f, 18.0f, 30.0f }; return r[eq.rangeIndex.load() & 3]; }

        float freqToX (float f) const { return graph.getX() + graph.getWidth() * std::log (f / kMinFreq) / std::log (kMaxFreq / kMinFreq); }
        float xToFreq (float x) const { return kMinFreq * std::pow (kMaxFreq / kMinFreq, juce::jlimit (0.0f, 1.0f, (x - graph.getX()) / graph.getWidth())); }
        float dbToY (float db) const { return graph.getCentreY() - db / currentRange() * (graph.getHeight() * 0.5f); }
        float yToDb (float y) const { return (graph.getCentreY() - y) / (graph.getHeight() * 0.5f) * currentRange(); }
        float specToY (float dbfs) const { return graph.getBottom() - (dbfs + 100.0f) / 100.0f * graph.getHeight(); }

        juce::Point<float> nodePos (int i) const
        {
            auto& b = eq.bands[(size_t) i];
            const int type = b.type->getIndex();
            const float db = (type == 3 || type == 4) ? 0.0f : (type == 5 ? 0.0f : b.gain->get());
            return { freqToX (b.freq->get()), dbToY (juce::jlimit (-currentRange() * 1.05f, currentRange() * 1.05f, db)) };
        }

        int hitNode (juce::Point<float> p) const
        {
            int best = -1;
            float bestD = 12.0f * 12.0f;
            for (int i = 0; i < kBands; ++i)
                if (eq.bands[(size_t) i].on->get())
                {
                    const auto c = nodePos (i);
                    const float d = (c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y);
                    if (d < bestD) { bestD = d; best = i; }
                }
            return best;
        }

        float bandDb (int i, float f) const
        {
            auto& b = eq.bands[(size_t) i];
            Coef c[4];
            const int n = designBand (b.type->getIndex(), eq.getRate() > 0 ? eq.getRate() : 48000.0, b.freq->get(), b.gain->get(), b.q->get(), b.slope->getIndex(), c);
            const double sr = eq.getRate() > 0 ? eq.getRate() : 48000.0;
            const float w = 2.0f * pi * f / (float) sr;
            float db = 0.0f;
            for (int k = 0; k < n; ++k)
                db += sectionDb (c[k], juce::jmin (w, pi * 0.999f));
            return db;
        }

        void removeBand (int i)
        {
            if (i < 0)
                return;
            setParam (eq.bands[(size_t) i].on, 0.0f);
            if (selected == i)
                select (-1);
            repaint();
        }

        void showTypeMenu (int i)
        {
            juce::PopupMenu m;
            for (int t = 0; t < 6; ++t)
                m.addItem (t + 1, kTypeNames[t], true, eq.bands[(size_t) i].type->getIndex() == t);
            m.addSeparator();
            m.addItem (100, "Delete band");
            juce::Component::SafePointer<EqEditor> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options(), [safe, i] (int r)
            {
                if (safe == nullptr || r <= 0)
                    return;
                if (r == 100)
                    safe->removeBand (i);
                else
                    setParam (safe->eq.bands[(size_t) i].type, (float) (r - 1));
                safe->repaint();
            });
        }

        void select (int i)
        {
            selected = i;
            typeAtt.reset(); slopeAtt.reset(); onAtt.reset(); freqAtt.reset(); gainAtt.reset(); qAtt.reset();
            const bool show = i >= 0;
            for (auto* c : std::initializer_list<juce::Component*> { &onBtn, &typeBox, &freqS, &gainS, &qS, &delBtn })
                c->setVisible (show);
            slopeBox.setVisible (false);
            if (show)
            {
                auto& b = eq.bands[(size_t) i];
                onAtt = std::make_unique<juce::ButtonParameterAttachment> (*b.on, onBtn);
                typeAtt = std::make_unique<juce::ComboBoxParameterAttachment> (*b.type, typeBox);
                slopeAtt = std::make_unique<juce::ComboBoxParameterAttachment> (*b.slope, slopeBox);
                freqAtt = std::make_unique<juce::SliderParameterAttachment> (*b.freq, freqS);
                gainAtt = std::make_unique<juce::SliderParameterAttachment> (*b.gain, gainS);
                qAtt = std::make_unique<juce::SliderParameterAttachment> (*b.q, qS);
            }
            repaint();
        }

        void refreshButtons()
        {
            static const char* modes[] = { "Analyzer: Pre+Post", "Analyzer: Pre", "Analyzer: Post", "Analyzer: Off" };
            analyzerBtn.setButtonText (modes[eq.analyzerMode.load() & 3]);
            rangeBtn.setButtonText (juce::String ((int) currentRange()) + " dB");
            repaint();
        }

        void drawSpectrum (juce::Graphics& g, const std::vector<float>& db, juce::Colour fillColour, juce::Colour lineColour, bool filled)
        {
            if (db.empty())
                return;
            const double sr = eq.getRate() > 0 ? eq.getRate() : 48000.0;
            const float binHz = (float) sr / (float) Fft::N;
            juce::Path p;
            bool started = false;
            float prevBin = 1.0f;
            for (int x = 0; x <= (int) graph.getWidth(); x += 2)
            {
                const float f = xToFreq (graph.getX() + (float) x);
                const float bin = f / binHz;
                if (bin < 1.0f || bin >= (float) db.size() - 1)
                    continue;
                float v = -200.0f;
                const int lo = (int) std::floor (prevBin), hi = (int) std::ceil (bin);
                for (int k = lo; k <= hi && k < (int) db.size(); ++k)
                    v = juce::jmax (v, db[(size_t) k]);
                if (hi - lo <= 1)   // low frequencies: interpolate between bins
                {
                    const int k = (int) bin;
                    const float fr = bin - (float) k;
                    v = db[(size_t) k] * (1.0f - fr) + db[(size_t) k + 1] * fr;
                }
                prevBin = bin;
                const float tilt = 4.5f * std::log2 (f / 1000.0f);   // pink-noise slope compensation
                const float y = specToY (juce::jlimit (-100.0f, 0.0f, v + tilt));
                if (! started) { p.startNewSubPath (graph.getX() + (float) x, y); started = true; }
                else p.lineTo (graph.getX() + (float) x, y);
            }
            if (! started)
                return;
            if (filled)
            {
                juce::Path f2 = p;
                f2.lineTo (graph.getRight(), graph.getBottom());
                f2.lineTo (graph.getX(), graph.getBottom());
                f2.closeSubPath();
                g.setColour (fillColour);
                g.fillPath (f2);
            }
            g.setColour (lineColour);
            g.strokePath (p, juce::PathStrokeType (1.2f));
        }

        void updateSpectrum (SampleFifo& fifo, std::vector<float>& ring, std::vector<float>& db)
        {
            float tmp[4096];
            const int got = fifo.pull (tmp, 4096);
            if (got > 0)
            {
                if (got >= Fft::N)
                    std::copy (tmp + got - Fft::N, tmp + got, ring.begin());
                else
                {
                    std::copy (ring.begin() + got, ring.end(), ring.begin());
                    std::copy (tmp, tmp + got, ring.end() - got);
                }
            }
            std::vector<float> now ((size_t) Fft::N / 2);
            fft.run (ring.data(), now.data());
            if (db.size() != now.size())
                db.assign (now.size(), -120.0f);
            for (size_t i = 0; i < now.size(); ++i)
                db[i] = now[i] > db[i] ? now[i] : db[i] * 0.85f + now[i] * 0.15f - 0.4f;   // fast attack, slow fall
        }

        void timerCallback() override
        {
            if (eq.analyzerMode.load() != 3)
            {
                updateSpectrum (eq.pre, preRing, preDb);
                updateSpectrum (eq.post, postRing, postDb);
            }
            else
            {
                float tmp[4096];
                while (eq.pre.pull (tmp, 4096) > 0) {}
                while (eq.post.pull (tmp, 4096) > 0) {}
            }
            if (selected >= 0)
            {
                const int type = eq.bands[(size_t) selected].type->getIndex();
                const bool cut = type == 3 || type == 4;
                if (slopeBox.isVisible() != cut)
                    slopeBox.setVisible (cut);
                const bool noGain = cut || type == 5;
                if (gainS.isEnabled() == noGain)
                    gainS.setEnabled (! noGain);
                gainS.setAlpha (noGain ? 0.35f : 1.0f);
            }
            repaint();
        }

        EqPlugin& eq;
        int selected = -1, dragging = -1, hover = -1;
        juce::Rectangle<float> graph;
        juce::Rectangle<int> panel, header, footer;

        juce::Colour bg { 0xff15171c }, bg2 { 0xff262b36 }, border { 0xff2a2f3a }, accent { 0xffff9a2e }, text { 0xffe8eaf0 }, muted { 0xff8b93a7 };

        juce::TextButton analyzerBtn, rangeBtn, delBtn;
        juce::ToggleButton onBtn;
        juce::ComboBox typeBox, slopeBox;
        juce::Slider freqS, gainS, qS, outS;
        std::unique_ptr<juce::ButtonParameterAttachment> onAtt;
        std::unique_ptr<juce::ComboBoxParameterAttachment> typeAtt, slopeAtt;
        std::unique_ptr<juce::SliderParameterAttachment> freqAtt, gainAtt, qAtt, outAtt;

        Fft fft;
        std::vector<float> preRing = std::vector<float> ((size_t) Fft::N, 0.0f), postRing = std::vector<float> ((size_t) Fft::N, 0.0f);
        std::vector<float> preDb, postDb;
    };

    juce::AudioProcessorEditor* EqPlugin::createEditor() { return new EqEditor (*this); }

    // ------------------------------------------------------------ compressor in the style of Waves H-Comp
    // (without the Analog and Punch sections). Zero latency: feed-forward, peak detector, no look-ahead, no oversampling.
    // Threshold 0..-48 dB, Ratio 1..50, Attack 0.5..100 ms, Release in ms or in note lengths (1/64 .. 2 bars, from a BPM
    // value or from the host tempo), Output +-18 dB, Mix = dry/wet (parallel compression), Limiter = 0 dBFS safety ceiling.
    const char* const kRelNotes[] = { "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bar" };
    const double kRelBeats[] = { 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1.0, 2.0, 4.0, 8.0 }; // in quarter notes

    class CompEditor;

    class CompPlugin : public BuiltinBase
    {
    public:
        CompPlugin() : BuiltinBase ("comp", "Compressor", "Hybrid compressor (zero latency)")
        {
            thresh = addLinear ("th", "Threshold", -48.0f, 0.0f, -18.0f);
            ratio = addFloat ("ra", "Ratio", 1.0f, 50.0f, 4.0f, 0.12f);
            attack = addFloat ("at", "Attack", 0.5f, 100.0f, 10.0f, 0.15f);
            release = addFloat ("re", "Release", 3.0f, 2000.0f, 120.0f, 0.1f);
            relNote = new juce::AudioParameterChoice (juce::ParameterID { "rn", 1 }, "Release Note", juce::StringArray { kRelNotes[0], kRelNotes[1], kRelNotes[2], kRelNotes[3], kRelNotes[4], kRelNotes[5], kRelNotes[6], kRelNotes[7] }, 3);
            juce::AudioProcessor::addParameter (relNote);
            sync = new juce::AudioParameterChoice (juce::ParameterID { "sy", 1 }, "Sync", juce::StringArray { "MS", "BPM", "HOST" }, 0);
            juce::AudioProcessor::addParameter (sync);
            bpm = addLinear ("bp", "BPM", 20.0f, 300.0f, 120.0f);
            output = addLinear ("ou", "Output", -18.0f, 18.0f, 0.0f);
            mix = addLinear ("mx", "Mix", 0.0f, 100.0f, 100.0f);
            limiter = new juce::AudioParameterBool (juce::ParameterID { "li", 1 }, "Limiter", false);
            juce::AudioProcessor::addParameter (limiter);
            meter = new juce::AudioParameterChoice (juce::ParameterID { "me", 1 }, "Meter", juce::StringArray { "IN", "GR", "OUT" }, 1);
            juce::AudioProcessor::addParameter (meter);
        }

        void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buf.getNumSamples(), nch = juce::jmin (2, buf.getNumChannels());
            if (nch == 0)
                return;

            const float th = thresh->get(), ra = ratio->get(), outG = juce::Decibels::decibelsToGain (output->get());
            const float wet = mix->get() * 0.01f, dry = 1.0f - wet;
            const bool lim = limiter->get();

            // release time in ms
            double relMs = release->get();
            const int sy = sync->getIndex();
            if (sy != 0)
            {
                double tempo = bpm->get();
                if (sy == 2)
                    if (auto* ph = getPlayHead())
                        if (auto pos = ph->getPosition())
                            if (auto b = pos->getBpm())
                                if (*b > 1.0)
                                    tempo = *b;
                relMs = juce::jlimit (1.0, 4000.0, kRelBeats[relNote->getIndex()] * 60000.0 / tempo);
            }

            const float atkC = std::exp (-1.0f / (float) (sr * 0.001 * (lim ? juce::jmin (0.5f, attack->get()) : attack->get())));
            const float relC = std::exp (-1.0f / (float) (sr * 0.001 * relMs));
            const float limRelC = std::exp (-1.0f / (float) (sr * 0.05));
            const float slope = 1.0f / (lim ? juce::jmax (ra, 20.0f) : ra) - 1.0f;
            constexpr float knee = 6.0f;

            float peakIn = 0.0f, peakOut = 0.0f, minGain = 0.0f;
            auto* d0 = buf.getWritePointer (0);
            auto* d1 = nch > 1 ? buf.getWritePointer (1) : nullptr;

            for (int i = 0; i < n; ++i)
            {
                const float l = d0[i], r = d1 ? d1[i] : l;
                const float level = juce::jmax (std::abs (l), std::abs (r));
                peakIn = juce::jmax (peakIn, level);

                const float lvlDb = 20.0f * std::log10 (juce::jmax (level, 1.0e-6f));
                const float over = lvlDb - th;
                float target = 0.0f; // wanted gain change in dB (<= 0)
                if (2.0f * over > knee)
                    target = slope * over;
                else if (2.0f * over > -knee)
                {
                    const float x = over + knee * 0.5f;
                    target = slope * x * x / (2.0f * knee);
                }

                // smoothing of the reduction (attack when it grows, release when it falls)
                const float c = target < grDb ? atkC : relC;
                grDb = target + c * (grDb - target);
                minGain = juce::jmin (minGain, grDb);

                float g = juce::Decibels::decibelsToGain (grDb) * outG;

                // 0 dBFS safety ceiling of the limiter button
                float lg = 1.0f;
                if (lim)
                {
                    const float pk = level * juce::Decibels::decibelsToGain (grDb) * outG;
                    const float need = pk > 1.0f ? 1.0f / pk : 1.0f;
                    limGain = need < limGain ? need : need + limRelC * (limGain - need);
                    lg = limGain;
                }
                else
                    limGain = 1.0f;

                const float yl = (l * g * lg) * wet + l * dry * outG;
                const float yr = (r * g * lg) * wet + r * dry * outG;
                d0[i] = yl;
                if (d1) d1[i] = yr;
                peakOut = juce::jmax (peakOut, juce::jmax (std::abs (yl), std::abs (yr)));
            }

            inLevel.store (peakIn);
            outLevel.store (peakOut);
            reduction.store (grDb);
        }

        juce::AudioProcessorEditor* createEditor() override;

        juce::AudioParameterFloat *thresh, *ratio, *attack, *release, *bpm, *output, *mix;
        juce::AudioParameterChoice *relNote, *sync, *meter;
        juce::AudioParameterBool* limiter;
        std::atomic<float> inLevel { 0.0f }, outLevel { 0.0f }, reduction { 0.0f };

    protected:
        void prepare() override { grDb = 0.0f; limGain = 1.0f; }

    private:
        float grDb = 0.0f, limGain = 1.0f;
    };

    class CompEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit CompEditor (CompPlugin& p) : juce::AudioProcessorEditor (p), plug (p)
        {
            setSize (640, 470);
            auto addKnob = [this] (juce::Slider& s, juce::Label& l, const juce::String& text)
            {
                s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
                s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 84, 18);
                s.setColour (juce::Slider::rotarySliderFillColourId, accent);
                s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3a4152));
                s.setColour (juce::Slider::thumbColourId, accent);
                s.setColour (juce::Slider::textBoxTextColourId, text_);
                s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
                addAndMakeVisible (s);
                l.setText (text, juce::dontSendNotification);
                l.setJustificationType (juce::Justification::centred);
                l.setColour (juce::Label::textColourId, muted);
                l.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                addAndMakeVisible (l);
            };
            addKnob (thS, thL, "THRESHOLD");
            addKnob (raS, raL, "RATIO");
            addKnob (atS, atL, "ATTACK");
            addKnob (reS, reL, "RELEASE");
            addKnob (rnS, rnL, "RELEASE");
            addKnob (ouS, ouL, "OUTPUT");
            addKnob (mxS, mxL, "MIX");
            addKnob (bpS, bpL, "BPM");

            thS.setRange (0.0, 48.0, 0.0);
            thS.setDoubleClickReturnValue (true, 18.0);
            thS.valueFromTextFunction = [] (const juce::String& t) { return (double) std::abs (t.getDoubleValue()); };
            thA = std::make_unique<juce::ParameterAttachment> (*p.thresh, [this] (float v) { thS.setValue (-v, juce::dontSendNotification); thS.updateText(); });
            thS.onDragStart = [this] { thA->beginGesture(); };
            thS.onDragEnd = [this] { thA->endGesture(); };
            thS.onValueChange = [this] { thA->setValueAsPartOfGesture ((float) -thS.getValue()); };
            thA->sendInitialUpdate();
            raA = std::make_unique<juce::SliderParameterAttachment> (*p.ratio, raS);
            atA = std::make_unique<juce::SliderParameterAttachment> (*p.attack, atS);
            reA = std::make_unique<juce::SliderParameterAttachment> (*p.release, reS);
            rnA = std::make_unique<juce::SliderParameterAttachment> (*p.relNote, rnS);
            ouA = std::make_unique<juce::SliderParameterAttachment> (*p.output, ouS);
            mxA = std::make_unique<juce::SliderParameterAttachment> (*p.mix, mxS);
            bpA = std::make_unique<juce::SliderParameterAttachment> (*p.bpm, bpS);

            thS.textFromValueFunction = [] (double v) { return juce::String (-v, 1) + " dB"; };
            raS.textFromValueFunction = [] (double v) { return v < 1.05 ? juce::String ("1 : 1") : juce::String (v, 1) + " : 1"; };
            atS.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " ms"; };
            reS.textFromValueFunction = [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 2) + " s" : juce::String ((int) v) + " ms"; };
            rnS.textFromValueFunction = [] (double v) { return juce::String (kRelNotes[juce::jlimit (0, 7, (int) std::round (v))]); };
            ouS.textFromValueFunction = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; };
            mxS.textFromValueFunction = [] (double v) { return juce::String ((int) std::round (v)) + " %"; };
            bpS.textFromValueFunction = [] (double v) { return juce::String (v, 1); };
            for (auto* s : { &thS, &raS, &atS, &reS, &rnS, &ouS, &mxS, &bpS })
                s->updateText();
            rnS.setNumDecimalPlacesToDisplay (0);

            for (auto* b : { &syncMs, &syncBpm, &syncHost })
            {
                b->setClickingTogglesState (false);
                b->setColour (juce::TextButton::buttonColourId, bg2);
                b->setColour (juce::TextButton::buttonOnColourId, accent);
                b->setColour (juce::TextButton::textColourOffId, text_);
                b->setColour (juce::TextButton::textColourOnId, juce::Colour (0xff15171c));
                addAndMakeVisible (b);
            }
            syncMs.setButtonText ("MS");
            syncBpm.setButtonText ("BPM");
            syncHost.setButtonText ("HOST");
            syncMs.onClick = [this] { setChoice (*plug.sync, 0); };
            syncBpm.onClick = [this] { setChoice (*plug.sync, 1); };
            syncHost.onClick = [this] { setChoice (*plug.sync, 2); };

            for (auto* b : { &mIn, &mGr, &mOut })
            {
                b->setClickingTogglesState (false);
                b->setColour (juce::TextButton::buttonColourId, bg2);
                b->setColour (juce::TextButton::buttonOnColourId, accent);
                b->setColour (juce::TextButton::textColourOffId, text_);
                b->setColour (juce::TextButton::textColourOnId, juce::Colour (0xff15171c));
                addAndMakeVisible (b);
            }
            mIn.setButtonText ("IN");
            mGr.setButtonText ("GR");
            mOut.setButtonText ("OUT");
            mIn.onClick = [this] { setChoice (*plug.meter, 0); };
            mGr.onClick = [this] { setChoice (*plug.meter, 1); };
            mOut.onClick = [this] { setChoice (*plug.meter, 2); };

            limBtn.setButtonText ("LIMITER");
            limBtn.setClickingTogglesState (true);
            limBtn.setColour (juce::TextButton::buttonColourId, bg2);
            limBtn.setColour (juce::TextButton::buttonOnColourId, accent);
            limBtn.setColour (juce::TextButton::textColourOffId, text_);
            limBtn.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff15171c));
            addAndMakeVisible (limBtn);
            limA = std::make_unique<juce::ButtonParameterAttachment> (*p.limiter, limBtn);

            startTimerHz (60);
            timerCallback();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (bg);
            g.setColour (text_);
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText ("COMPRESSOR", 16, 8, 200, 26, juce::Justification::centredLeft);

            // VU face
            auto f = meterArea.toFloat();
            g.setColour (juce::Colour (0xff0d0f13));
            g.fillRoundedRectangle (f.expanded (4.0f), 8.0f);
            juce::ColourGradient face (juce::Colour (0xfff4e8c4), f.getCentreX(), f.getBottom(), juce::Colour (0xffcdb784), f.getX(), f.getY(), true);
            g.setGradientFill (face);
            g.fillRoundedRectangle (f, 6.0f);

            const int mode = plug.meter->getIndex();
            const float cx = f.getCentreX(), cy = f.getBottom() + f.getHeight() * 0.55f, rad = f.getHeight() * 1.45f;
            const float a0 = -0.62f, a1 = 0.62f; // radians from vertical
            auto ang = [&] (float t) { return a0 + (a1 - a0) * t; };
            auto pt = [&] (float t, float r) { return juce::Point<float> (cx + std::sin (ang (t)) * r, cy - std::cos (ang (t)) * r); };

            g.saveState();
            g.reduceClipRegion (meterArea);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            const int ticks = mode == 1 ? 20 : 43;
            for (int i = 0; i <= ticks; ++i)
            {
                // GR: 0 dB on the left, 20 dB on the right (more reduction = further right); IN/OUT: -40 dB on the left, +3 dB on the right
                const float t = (float) i / (float) ticks;
                const bool major = mode == 1 ? (i % 5 == 0) : (i % 10 == 3 || i == ticks);
                const bool red = mode == 1 ? false : (i >= 40);
                g.setColour (red ? juce::Colour (0xffc0392b) : juce::Colour (0xff2b2417));
                g.drawLine (juce::Line<float> (pt (t, rad - (major ? 16.0f : 9.0f)), pt (t, rad - 2.0f)), major ? 2.0f : 1.0f);
                if (major)
                {
                    const int val = mode == 1 ? i : -40 + i;
                    auto tp = pt (t, rad - 30.0f);
                    g.setColour (red ? juce::Colour (0xffc0392b) : juce::Colour (0xff2b2417));
                    g.drawText (mode == 1 ? juce::String (val) : (val > 0 ? "+" + juce::String (val) : juce::String (val)),
                                juce::Rectangle<float> (30.0f, 14.0f).withCentre (tp), juce::Justification::centred);
                }
            }
            g.setColour (juce::Colour (0xff2b2417));
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (mode == 1 ? "GAIN REDUCTION  dB" : (mode == 0 ? "INPUT  dB" : "OUTPUT  dB"),
                        meterArea.withTrimmedTop (meterArea.getHeight() / 2).withTrimmedBottom (meterArea.getHeight() / 4 - 6), juce::Justification::centred);

            // needle
            const float t = juce::jlimit (0.0f, 1.0f, needle);
            auto tip = pt (t, rad - 3.0f);
            auto base = pt (t, rad - f.getHeight() * 1.05f);
            g.setColour (juce::Colour (0x40000000));
            g.drawLine (juce::Line<float> (base.translated (2, 2), tip.translated (2, 2)), 3.0f);
            g.setColour (juce::Colour (0xffb0201a));
            g.drawLine (juce::Line<float> (base, tip), 2.2f);
            g.restoreState();

            g.setColour (juce::Colour (0xff1a1d24));
            g.fillRect (meterArea.getX(), meterArea.getBottom() - 10, meterArea.getWidth(), 10);

            // numeric read-out
            g.setColour (muted);
            g.setFont (juce::FontOptions (12.0f));
            const float shownDb = mode == 1 ? grShown : juce::Decibels::gainToDecibels (mode == 0 ? inShown : outShown, -60.0f);
            g.drawText (juce::String (shownDb, 1) + " dB", juce::Rectangle<int> (meterArea.getX(), meterArea.getBottom() + 4, meterArea.getWidth(), 16), juce::Justification::centred);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (16, 8);
            auto top = r.removeFromTop (30);
            limBtn.setBounds (top.removeFromRight (84));
            top.removeFromRight (8);
            mOut.setBounds (top.removeFromRight (44));
            mGr.setBounds (top.removeFromRight (44));
            mIn.setBounds (top.removeFromRight (44));
            top.removeFromRight (8);
            juce::ignoreUnused (top);
            meterArea = juce::Rectangle<int> (getWidth() / 2 - 180, 46, 360, 130);
            r.removeFromTop (172);
            auto row1 = r.removeFromTop (118);
            const int w = row1.getWidth() / 4;
            for (auto pr : { std::pair<juce::Slider*, juce::Label*> { &thS, &thL }, { &raS, &raL }, { &atS, &atL }, { &reS, &reL } })
            {
                auto c = row1.removeFromLeft (w);
                pr.second->setBounds (c.removeFromTop (18));
                pr.first->setBounds (c);
                if (pr.first == &reS)
                {
                    rnL.setBounds (pr.second->getBounds());
                    rnS.setBounds (pr.first->getBounds());
                }
            }
            r.removeFromTop (8);
            auto row2 = r.removeFromTop (118);
            const int w2 = row2.getWidth() / 4;
            auto c1 = row2.removeFromLeft (w2);
            ouL.setBounds (c1.removeFromTop (18)); ouS.setBounds (c1);
            auto c2 = row2.removeFromLeft (w2);
            mxL.setBounds (c2.removeFromTop (18)); mxS.setBounds (c2);
            auto c3 = row2.removeFromLeft (w2);
            bpL.setBounds (c3.removeFromTop (18)); bpS.setBounds (c3);
            auto c4 = row2.reduced (10, 6);
            syncMs.setBounds (c4.removeFromTop (28));
            c4.removeFromTop (6);
            syncBpm.setBounds (c4.removeFromTop (28));
            c4.removeFromTop (6);
            syncHost.setBounds (c4.removeFromTop (28));
        }

    private:
        void setChoice (juce::AudioParameterChoice& c, int i)
        {
            c.beginChangeGesture();
            c = i;
            c.endChangeGesture();
        }

        void timerCallback() override
        {
            const int sy = plug.sync->getIndex(), me = plug.meter->getIndex();
            syncMs.setToggleState (sy == 0, juce::dontSendNotification);
            syncBpm.setToggleState (sy == 1, juce::dontSendNotification);
            syncHost.setToggleState (sy == 2, juce::dontSendNotification);
            mIn.setToggleState (me == 0, juce::dontSendNotification);
            mGr.setToggleState (me == 1, juce::dontSendNotification);
            mOut.setToggleState (me == 2, juce::dontSendNotification);
            reS.setVisible (sy == 0); reL.setVisible (sy == 0);
            rnS.setVisible (sy != 0); rnL.setVisible (sy != 0);
            bpS.setEnabled (sy == 1); bpL.setEnabled (sy == 1);
            bpS.setAlpha (sy == 1 ? 1.0f : 0.4f);
            bpL.setAlpha (sy == 1 ? 1.0f : 0.4f);

            // fast rise, slow fall
            auto follow = [] (float& shown, float now, bool gr)
            {
                if (gr ? now < shown : now > shown) shown = now;
                else shown += (now - shown) * 0.25f;
            };
            follow (inShown, plug.inLevel.load(), false);
            follow (outShown, plug.outLevel.load(), false);
            // the GR needle follows the real reduction (so attack and release are visible), the level needles are VU-like
            grShown += (plug.reduction.load() - grShown) * 0.6f;
            const int m = plug.meter->getIndex();
            const float target = m == 1 ? juce::jlimit (0.0f, 1.0f, -grShown / 20.0f)
                                        : juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (m == 0 ? inShown : outShown, -60.0f) + 40.0f) / 43.0f);
            needle += (target - needle) * (m == 1 ? 0.7f : 0.35f);
            repaint (meterArea.expanded (6, 24));
        }

        CompPlugin& plug;
        juce::Slider thS, raS, atS, reS, rnS, ouS, mxS, bpS;
        juce::Label thL, raL, atL, reL, rnL, ouL, mxL, bpL;
        std::unique_ptr<juce::ParameterAttachment> thA;
        std::unique_ptr<juce::SliderParameterAttachment> raA, atA, reA, rnA, ouA, mxA, bpA;
        juce::TextButton syncMs, syncBpm, syncHost, mIn, mGr, mOut, limBtn;
        std::unique_ptr<juce::ButtonParameterAttachment> limA;
        juce::Rectangle<int> meterArea;
        float inShown = 0.0f, outShown = 0.0f, grShown = 0.0f, needle = 0.0f;
        juce::Colour bg { 0xff15171c }, bg2 { 0xff262b36 }, border { 0xff2a2f3a }, accent { 0xffff9a2e }, text_ { 0xffe8eaf0 }, muted { 0xff8b93a7 };
    };

    juce::AudioProcessorEditor* CompPlugin::createEditor() { return new CompEditor (*this); }

    // ------------------------------------------------------------ noise gate (zero latency)
    // No look-ahead: the gate opens as fast as the Attack time allows (min 0.05 ms) and closes after Hold + Release.
    // The detector can be filtered (Low Cut / High Cut) so that rumble or hiss does not open the gate (e.g. stage bleed).
    class GatePlugin : public BuiltinBase
    {
    public:
        static constexpr float kHysteresis = 4.0f;   // dB between opening and closing, fixed: it only keeps the tail of a note from chattering

        GatePlugin() : BuiltinBase ("gate", "Gate", "Noise gate (zero latency)")
        {
            thresh = addLinear ("th", "Threshold", -80.0f, 0.0f, -45.0f);
            range = addLinear ("rg", "Range", -100.0f, 0.0f, -100.0f);
            attack = addFloat ("at", "Attack", 0.05f, 50.0f, 0.5f, 0.1f);
            hold = addFloat ("ho", "Hold", 0.0f, 1000.0f, 80.0f, 0.15f);
            release = addFloat ("re", "Release", 5.0f, 3000.0f, 150.0f, 0.1f);
            lowCut = addFloat ("lc", "Low Cut", 20.0f, 2000.0f, 20.0f, 0.1f);
            highCut = addFloat ("hc", "High Cut", 500.0f, 20000.0f, 20000.0f, 0.3f);
            ratio = addFloat ("ra", "Ratio", 1.5f, 100.0f, 10.0f, 0.1f);
        }

        void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buf.getNumSamples(), nch = juce::jmin (2, buf.getNumChannels());
            if (nch == 0)
                return;

            const float th = thresh->get(), hy = kHysteresis, rangeDb = range->get(), expand = ratio->get() - 1.0f;
            const int holdSamples = (int) (sr * 0.001 * hold->get());
            const float atkC = std::exp (-1.0f / (float) (sr * 0.001 * attack->get()));
            const float relC = std::exp (-1.0f / (float) (sr * 0.001 * release->get()));

            if (lastLc != lowCut->get() || lastHc != highCut->get())
            {
                lastLc = lowCut->get();
                lastHc = highCut->get();
                hp.makeHighPass (sr, lastLc);
                lp.makeLowPass (sr, juce::jmin (lastHc, (float) (sr * 0.45)));
            }
            const bool filtered = lastLc > 21.0f || lastHc < 19900.0f;

            float peakIn = 0.0f, peakOut = 0.0f;
            auto* d0 = buf.getWritePointer (0);
            auto* d1 = nch > 1 ? buf.getWritePointer (1) : nullptr;

            for (int i = 0; i < n; ++i)
            {
                const float l = d0[i], r = d1 ? d1[i] : l;
                peakIn = juce::jmax (peakIn, juce::jmax (std::abs (l), std::abs (r)));

                float dl = l, dr = r;
                if (filtered)
                {
                    dl = lp.process (0, hp.process (0, l));
                    dr = d1 ? lp.process (1, hp.process (1, r)) : dl;
                }
                const float lvlDb = 20.0f * std::log10 (juce::jmax (juce::jmax (std::abs (dl), std::abs (dr)), 1.0e-6f));

                if (lvlDb > th)
                {
                    open = true;
                    holdLeft = holdSamples;
                }
                else if (lvlDb < th - hy)
                {
                    if (holdLeft > 0)
                        --holdLeft;
                    else
                        open = false;
                }
                else if (open)
                    holdLeft = juce::jmax (holdLeft, 0);

                // below the threshold the gate expands downwards with the ratio, but never deeper than Range
                const float target = open ? 1.0f : juce::Decibels::decibelsToGain (juce::jmax (rangeDb, -(th - lvlDb) * expand), -90.0f);
                const float c = target > gain ? atkC : relC;
                gain = target + c * (gain - target);

                d0[i] = l * gain;
                if (d1) d1[i] = r * gain;
                peakOut = juce::jmax (peakOut, juce::jmax (std::abs (d0[i]), d1 ? std::abs (d1[i]) : 0.0f));
            }

            inLevel.store (peakIn);
            outLevel.store (peakOut);
            reduction.store (juce::Decibels::gainToDecibels (gain, -90.0f));
            isOpen.store (open);
        }

        juce::AudioProcessorEditor* createEditor() override;

        juce::AudioParameterFloat *thresh, *range, *attack, *hold, *release, *lowCut, *highCut, *ratio;
        std::atomic<float> inLevel { 0.0f }, outLevel { 0.0f }, reduction { 0.0f };
        std::atomic<bool> isOpen { false };

    protected:
        void prepare() override
        {
            gain = 1.0f; open = false; holdLeft = 0; lastLc = lastHc = -1.0f;
            hp.reset(); lp.reset();
        }

    private:
        Biquad hp, lp;
        float gain = 1.0f, lastLc = -1.0f, lastHc = -1.0f;
        bool open = false;
        int holdLeft = 0;
    };

    class GateEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit GateEditor (GatePlugin& p) : juce::AudioProcessorEditor (p), plug (p)
        {
            setSize (760, 500);
            addRow (atS, atL, "ATTACK", *p.attack, atA);
            addRow (hoS, hoL, "HOLD", *p.hold, hoA);
            addRow (reS, reL, "RELEASE", *p.release, reA);
            addRow (thS, thL, "THRESHOLD", *p.thresh, thA);
            addRow (raS, raL, "RATIO", *p.ratio, raA);
            addRow (rgS, rgL, "RANGE", *p.range, rgA);
            addRow (lcS, lcL, "KEY LOW CUT", *p.lowCut, lcA);
            addRow (hcS, hcL, "KEY HIGH CUT", *p.highCut, hcA);

            auto ms = [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 2) + " s" : (v < 10.0 ? juce::String (v, 2) : juce::String (v, 0)) + " ms"; };
            auto hz = [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 1) + " kHz" : juce::String ((int) v) + " Hz"; };
            thS.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " dB"; };
            rgS.textFromValueFunction = [] (double v) { return juce::String (-v, 1) + " dB"; };
            raS.textFromValueFunction = [] (double v) { return v >= 99.0 ? juce::String ("1:inf") : "1:" + juce::String (v, 1); };
            atS.textFromValueFunction = ms;
            hoS.textFromValueFunction = ms;
            reS.textFromValueFunction = ms;
            lcS.textFromValueFunction = hz;
            hcS.textFromValueFunction = hz;
            for (auto* s : { &thS, &rgS, &atS, &hoS, &reS, &lcS, &hcS, &raS })
                s->updateText();

            startTimerHz (60);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (bg);
            g.setColour (text_);
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText ("GATE", 20, 10, 120, 28, juce::Justification::centredLeft);

            const bool o = plug.isOpen.load();
            g.setColour (o ? juce::Colour (0xff4cd964) : juce::Colour (0xff3a4152));
            g.fillEllipse (110.0f, 18.0f, 12.0f, 12.0f);
            g.setColour (muted);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (o ? "OPEN" : "CLOSED", 128, 10, 70, 28, juce::Justification::centredLeft);

            paintTransfer (g);
            paintKeyFilter (g);
        }

        void resized() override
        {
            transferArea = juce::Rectangle<int> (16, 48, 340, 290);
            filterArea = juce::Rectangle<int> (16, 346, 340, 140);

            juce::Slider* sl[8] = { &thS, &raS, &rgS, &atS, &hoS, &reS, &lcS, &hcS };
            juce::Label* lb[8] = { &thL, &raL, &rgL, &atL, &hoL, &reL, &lcL, &hcL };
            for (int i = 0; i < 8; ++i)
            {
                auto row = juce::Rectangle<int> (372, 48 + i * 36, 372, 36);
                lb[i]->setBounds (row.removeFromLeft (104));
                sl[i]->setBounds (row);
            }
        }

        // the threshold is dragged on the transfer graph, the two key filters on the frequency graph
        void mouseDown (const juce::MouseEvent& e) override
        {
            drag = None;
            const auto p = e.position;
            if (std::abs (p.x - thresholdX()) < 9.0f && p.y >= plotRect().getY() - 6.0f && p.y <= plotRect().getBottom() + 6.0f)
                drag = Threshold;
            else if (filterPlot().expanded (6.0f).contains (p))
            {
                const float dl = std::abs (p.x - freqX ((float) plug.lowCut->get())), dh = std::abs (p.x - freqX ((float) plug.highCut->get()));
                if (juce::jmin (dl, dh) < 12.0f)
                    drag = dl <= dh ? LowCut : HighCut;
            }
            if (drag != None)
            {
                auto& prm = paramFor (drag);
                prm.beginChangeGesture();
                mouseDrag (e);
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (drag == None)
                return;
            auto& prm = paramFor (drag);
            const auto range = prm.getNormalisableRange();
            float value = 0.0f;
            if (drag == Threshold)
                value = (e.position.x - plotRect().getX()) / plotRect().getWidth() * 100.0f - 100.0f;
            else
                value = xFreq (e.position.x);
            value = juce::jlimit (range.start, range.end, value);
            prm.setValueNotifyingHost (prm.convertTo0to1 (value));
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (drag != None)
                paramFor (drag).endChangeGesture();
            drag = None;
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const auto p = e.position;
            bool hand = std::abs (p.x - thresholdX()) < 9.0f && p.y >= plotRect().getY() - 6.0f && p.y <= plotRect().getBottom() + 6.0f;
            if (! hand && filterPlot().expanded (6.0f).contains (p))
                hand = std::abs (p.x - freqX ((float) plug.lowCut->get())) < 12.0f || std::abs (p.x - freqX ((float) plug.highCut->get())) < 12.0f;
            setMouseCursor (hand ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
        }

    private:
        enum Drag { None, Threshold, LowCut, HighCut };

        juce::RangedAudioParameter& paramFor (Drag d) { return d == Threshold ? (juce::RangedAudioParameter&) *plug.thresh : d == LowCut ? (juce::RangedAudioParameter&) *plug.lowCut : (juce::RangedAudioParameter&) *plug.highCut; }

        juce::Rectangle<float> plotRect() const { return { (float) transferArea.getX() + 84.0f, (float) transferArea.getY() + 4.0f, 252.0f, 252.0f }; }
        float dbX (float db) const { return plotRect().getX() + (juce::jlimit (-100.0f, 0.0f, db) + 100.0f) / 100.0f * plotRect().getWidth(); }
        float thresholdX() const { return dbX (plug.thresh->get()); }

        // frequency graph: 20 Hz .. 20 kHz on a log axis, +6 .. -36 dB
        juce::Rectangle<float> filterPlot() const { return { (float) filterArea.getX() + 40.0f, (float) filterArea.getY() + 8.0f, (float) filterArea.getWidth() - 56.0f, (float) filterArea.getHeight() - 32.0f }; }
        float freqX (float f) const
        {
            const auto r = filterPlot();
            return r.getX() + std::log (juce::jlimit (20.0f, 20000.0f, f) / 20.0f) / std::log (1000.0f) * r.getWidth();
        }
        float xFreq (float x) const
        {
            const auto r = filterPlot();
            return 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth()));
        }
        float filterY (float db) const
        {
            const auto r = filterPlot();
            return r.getY() + (6.0f - juce::jlimit (-36.0f, 6.0f, db)) / 42.0f * r.getHeight();
        }

        void paintKeyFilter (juce::Graphics& g)
        {
            g.setColour (juce::Colour (0xff0d0f13));
            g.fillRect (filterArea.toFloat());
            const auto r = filterPlot();

            g.setFont (juce::FontOptions (10.0f));
            for (float f : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
            {
                g.setColour (juce::Colour (0xff1f2430));
                g.drawVerticalLine ((int) freqX (f), r.getY(), r.getBottom());
                g.setColour (muted);
                const juce::String t = f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k" : juce::String ((int) f);
                if (f == 20.0f || f == 100.0f || f == 1000.0f || f == 10000.0f || f == 20000.0f)
                    g.drawText (t, juce::Rectangle<float> (freqX (f) - 18.0f, r.getBottom() + 3.0f, 36.0f, 12.0f), juce::Justification::centred);
            }
            for (int db : { 0, -12, -24, -36 })
            {
                g.setColour (juce::Colour (0xff1f2430));
                g.drawHorizontalLine ((int) filterY ((float) db), r.getX(), r.getRight());
                g.setColour (muted);
                g.drawText (juce::String (db), juce::Rectangle<float> (r.getX() - 34.0f, filterY ((float) db) - 6.0f, 30.0f, 12.0f), juce::Justification::centredRight);
            }
            g.setColour (juce::Colour (0xff3a4152));
            g.drawRect (r);

            // response of the two key filters (second order, Butterworth)
            const float lc = plug.lowCut->get(), hc = plug.highCut->get();
            juce::Path curve, fill;
            const int steps = 200;
            for (int i = 0; i <= steps; ++i)
            {
                const float x = r.getX() + r.getWidth() * (float) i / (float) steps;
                const float f = xFreq (x);
                const float hp = 1.0f / (1.0f + std::pow (lc / f, 4.0f));
                const float lp = 1.0f / (1.0f + std::pow (f / hc, 4.0f));
                const float db = 10.0f * std::log10 (juce::jmax (1.0e-6f, hp * lp));
                const auto pt = juce::Point<float> (x, filterY (db));
                if (i == 0) { curve.startNewSubPath (pt); fill.startNewSubPath (x, filterY (-36.0f)); fill.lineTo (pt); }
                else { curve.lineTo (pt); fill.lineTo (pt); }
            }
            fill.lineTo (r.getRight(), filterY (-36.0f));
            fill.closeSubPath();
            g.setColour (accent.withAlpha (0.16f));
            g.fillPath (fill);
            g.setColour (accent);
            g.strokePath (curve, juce::PathStrokeType (2.0f));

            // the two grips (-3 dB points)
            for (float f : { lc, hc })
            {
                g.setColour (text_);
                g.fillEllipse (freqX (f) - 5.0f, filterY (-3.0f) - 5.0f, 10.0f, 10.0f);
                g.setColour (accent);
                g.drawEllipse (freqX (f) - 5.0f, filterY (-3.0f) - 5.0f, 10.0f, 10.0f, 1.5f);
            }
            g.setColour (muted);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText ("KEY FILTER (dB)", juce::Rectangle<float> (r.getX() + 4.0f, r.getY() + 1.0f, 120.0f, 12.0f), juce::Justification::centredLeft);
        }

        void paintTransfer (juce::Graphics& g)
        {
            const auto plot = plotRect();
            g.setColour (juce::Colour (0xff0d0f13));
            g.fillRect (transferArea.toFloat());

            auto X = [&] (float db) { return dbX (db); };
            auto Y = [&] (float db) { return plot.getBottom() - (juce::jlimit (-100.0f, 0.0f, db) + 100.0f) / 100.0f * plot.getHeight(); };

            g.setFont (juce::FontOptions (11.0f));
            for (int db = 0; db >= -100; db -= 20)
            {
                g.setColour (juce::Colour (0xff1f2430));
                g.drawHorizontalLine ((int) Y ((float) db), plot.getX(), plot.getRight());
                g.drawVerticalLine ((int) X ((float) db), plot.getY(), plot.getBottom());
                g.setColour (muted);
                g.drawText (juce::String (db), juce::Rectangle<float> (plot.getX() - 36.0f, Y ((float) db) - 8.0f, 32.0f, 16.0f), juce::Justification::centredRight);
            }
            g.setColour (juce::Colour (0xff3a4152));
            g.drawRect (plot);

            // input level meter (left of the dB labels)
            auto m = juce::Rectangle<float> ((float) transferArea.getX() + 6.0f, plot.getY(), 14.0f, plot.getHeight());
            auto m2 = m.translated (20.0f, 0.0f);
            g.setColour (juce::Colour (0xff1a1d24));
            g.fillRect (m);
            g.fillRect (m2);
            const float inDb = juce::Decibels::gainToDecibels (inShown, -100.0f);
            g.setColour (accent);
            g.fillRect (m.withTop (Y (inDb)));
            // gain reduction meter: grows from the top downwards
            g.setColour (juce::Colour (0xff4cd964));
            g.fillRect (m2.withHeight (juce::jlimit (0.0f, 1.0f, -grShown / 100.0f) * m2.getHeight()));
            g.setColour (muted);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText ("IN", m.withY (plot.getBottom() + 2.0f).withHeight (14.0f).expanded (4.0f, 0.0f), juce::Justification::centred);
            g.drawText ("GR", m2.withY (plot.getBottom() + 2.0f).withHeight (14.0f).expanded (4.0f, 0.0f), juce::Justification::centred);

            // reference diagonal
            g.setColour (juce::Colour (0xff4a5163));
            g.drawLine (X (-100.0f), Y (-100.0f), X (0.0f), Y (0.0f), 1.0f);

            // transfer curve
            const float th = plug.thresh->get(), rg = plug.range->get(), ex = plug.ratio->get() - 1.0f;
            juce::Path path;
            for (int i = 0; i <= 100; ++i)
            {
                const float in = -100.0f + (float) i;
                const float out = in >= th ? in : in + juce::jmax (rg, -(th - in) * ex);
                const auto pt = juce::Point<float> (X (in), Y (out));
                if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
            }
            g.setColour (accent);
            g.strokePath (path, juce::PathStrokeType (2.5f));

            // threshold line with a grip: drag it with the mouse
            g.setColour (juce::Colour (0xc0ff9a2e));
            g.drawVerticalLine ((int) X (th), plot.getY(), plot.getBottom());
            juce::Path grip;
            grip.addTriangle (X (th) - 7.0f, plot.getY(), X (th) + 7.0f, plot.getY(), X (th), plot.getY() + 11.0f);
            g.setColour (accent);
            g.fillPath (grip);
            g.setColour (text_);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (juce::String (th, 1) + " dB", juce::Rectangle<float> (X (th) - 30.0f, plot.getBottom() - 18.0f, 60.0f, 14.0f), juce::Justification::centred);

            // live point
            const float outDb = inDb + grShown;
            g.setColour (juce::Colours::white);
            g.fillEllipse (X (inDb) - 4.5f, Y (outDb) - 4.5f, 9.0f, 9.0f);

            g.setColour (muted);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("INPUT (dB)", juce::Rectangle<float> (plot.getX(), plot.getBottom() + 2.0f, plot.getWidth(), 14.0f), juce::Justification::centredRight);
        }

        void addRow (juce::Slider& s, juce::Label& l, const juce::String& text, juce::RangedAudioParameter& prm, std::unique_ptr<juce::SliderParameterAttachment>& att)
        {
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 20);
            s.setColour (juce::Slider::trackColourId, accent);
            s.setColour (juce::Slider::thumbColourId, text_);
            s.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff3a4152));
            s.setColour (juce::Slider::textBoxTextColourId, accent);
            s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            addAndMakeVisible (s);
            l.setText (text, juce::dontSendNotification);
            l.setColour (juce::Label::textColourId, muted);
            l.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            addAndMakeVisible (l);
            att = std::make_unique<juce::SliderParameterAttachment> (prm, s);
        }

        void timerCallback() override
        {
            const float now = plug.inLevel.load();
            if (now > inShown) inShown = now; else inShown += (now - inShown) * 0.2f;
            grShown += (plug.reduction.load() - grShown) * 0.6f;
            repaint (transferArea);
            repaint (filterArea);
            repaint (100, 8, 120, 28);
        }

        GatePlugin& plug;
        juce::Slider thS, rgS, atS, hoS, reS, lcS, hcS, raS;
        juce::Label thL, rgL, atL, hoL, reL, lcL, hcL, raL;
        std::unique_ptr<juce::SliderParameterAttachment> thA, rgA, atA, hoA, reA, lcA, hcA, raA;
        juce::Rectangle<int> transferArea, filterArea;
        Drag drag = None;
        float inShown = 0.0f, grShown = 0.0f;
        juce::Colour bg { 0xff15171c }, bg2 { 0xff262b36 }, accent { 0xffff9a2e }, text_ { 0xffe8eaf0 }, muted { 0xff8b93a7 };
    };

    juce::AudioProcessorEditor* GatePlugin::createEditor() { return new GateEditor (*this); }

    // ------------------------------------------------------------ reverb in the style of Valhalla VintageVerb
    // Algorithmic reverb: stereo pre-delay -> input diffusion -> 8-line feedback delay network (Hadamard matrix,
    // damping in the loop, slowly modulated delay lines) -> low cut / high cut on the wet signal -> mix.
    // It adds NO latency: the dry signal passes straight through; the only delay is the Pre-Delay the user asks for.
    class ReverbPlugin : public BuiltinBase
    {
    public:
        ReverbPlugin() : BuiltinBase ("reverb", "Reverb", "Vintage-style algorithmic reverb (no latency)")
        {
            mode = new juce::AudioParameterChoice (juce::ParameterID { "mo", 1 }, "Mode", juce::StringArray { "Hall", "Plate", "Room" }, 0);
            juce::AudioProcessor::addParameter (mode);
            preDelay = addFloat ("pd", "Pre-Delay", 0.0f, 500.0f, 20.0f, 0.1f);
            decay = addFloat ("de", "Decay", 0.2f, 20.0f, 2.5f, 0.15f);
            lowCut = addFloat ("lc", "Low Cut", 20.0f, 1000.0f, 100.0f, 0.1f);
            highCut = addFloat ("hc", "High Cut", 1000.0f, 20000.0f, 10000.0f, 0.3f);
            damping = addLinear ("da", "Damping", 0.0f, 100.0f, 50.0f);
            tone = addLinear ("to", "Tone", -12.0f, 12.0f, 0.0f);
            mix = addLinear ("mx", "Mix", 0.0f, 100.0f, 25.0f);
        }

        double getTailLengthSeconds() const override { return 20.0; }

        void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buf.getNumSamples(), nch = juce::jmin (2, buf.getNumChannels());
            if (nch == 0)
                return;

            core.setParams (mode->getIndex(), preDelay->get(), decay->get(), damping->get() * 0.01f);
            if (lowCut->get() != lastLc || highCut->get() != lastHc)
            {
                lastLc = lowCut->get();
                lastHc = highCut->get();
                hp.makeHighPass (sr, lastLc);
                lp.makeLowPass (sr, juce::jmin (lastHc, (float) (sr * 0.45)));
            }
            if (tone->get() != lastTone)
            {
                // tilt around 1 kHz: bass up / treble down (or the other way round), 0 = flat
                lastTone = tone->get();
                tiltLow.makeLowShelf (sr, 1000.0f, -lastTone * 0.5f, 0.7f);
                tiltHigh.makeHighShelf (sr, 1000.0f, lastTone * 0.5f, 0.7f);
            }
            const float wetAmt = mix->get() * 0.01f, dryAmt = 1.0f - wetAmt;

            auto* d0 = buf.getWritePointer (0);
            auto* d1 = nch > 1 ? buf.getWritePointer (1) : nullptr;
            for (int s = 0; s < n; ++s)
            {
                const float inL = d0[s], inR = d1 ? d1[s] : inL;
                float wl, wr;
                core.process (inL, inR, wl, wr);
                wl = tiltHigh.process (0, tiltLow.process (0, lp.process (0, hp.process (0, wl))));
                wr = tiltHigh.process (1, tiltLow.process (1, lp.process (1, hp.process (1, wr))));
                d0[s] = inL * dryAmt + wl * wetAmt;
                if (d1) d1[s] = inR * dryAmt + wr * wetAmt;
            }
        }

        juce::AudioParameterChoice* mode;
        juce::AudioParameterFloat *preDelay, *decay, *lowCut, *highCut, *damping, *tone, *mix;
        juce::AudioProcessorEditor* createEditor() override;

    protected:
        void prepare() override
        {
            core.prepare (sr);
            lastLc = lastHc = lastTone = -99.0f;
            hp.reset(); lp.reset(); tiltLow.reset(); tiltHigh.reset();
        }

    private:
        tcrev::Core core;
        float lastLc = -99.0f, lastHc = -99.0f, lastTone = -99.0f;
        Biquad hp, lp, tiltLow, tiltHigh;
    };

    class ReverbEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit ReverbEditor (ReverbPlugin& p) : juce::AudioProcessorEditor (p), plug (p)
        {
            setSize (620, 330);
            addKnob (pdS, pdL, "PRE-DELAY", *p.preDelay, pdA);
            addKnob (deS, deL, "DECAY", *p.decay, deA);
            addKnob (lcS, lcL, "LOW CUT", *p.lowCut, lcA);
            addKnob (hcS, hcL, "HIGH CUT", *p.highCut, hcA);
            addKnob (daS, daL, "DAMPING", *p.damping, daA);
            addKnob (toS, toL, "TONE", *p.tone, toA);
            addKnob (mxS, mxL, "MIX", *p.mix, mxA);
            pdS.textFromValueFunction = [] (double v) { return juce::String ((int) std::round (v)) + " ms"; };
            deS.textFromValueFunction = [] (double v) { return juce::String (v, 2) + " s"; };
            lcS.textFromValueFunction = [] (double v) { return juce::String ((int) std::round (v)) + " Hz"; };
            hcS.textFromValueFunction = [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 1) + " kHz" : juce::String ((int) v) + " Hz"; };
            daS.textFromValueFunction = [] (double v) { return juce::String ((int) std::round (v)) + " %"; };
            toS.textFromValueFunction = [] (double v) { return std::abs (v) < 0.05 ? juce::String ("FLAT") : (v < 0 ? "DARK " : "BRIGHT ") + juce::String (std::abs (v), 1); };
            mxS.textFromValueFunction = [] (double v) { return juce::String ((int) std::round (v)) + " %"; };
            for (auto* s : { &pdS, &deS, &lcS, &hcS, &daS, &toS, &mxS })
                s->updateText();
            const char* names[3] = { "HALL", "PLATE", "ROOM" };
            for (int i = 0; i < 3; ++i)
            {
                auto& b = modeBtn[i];
                b.setButtonText (names[i]);
                b.setClickingTogglesState (false);
                b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff262b36));
                b.setColour (juce::TextButton::buttonOnColourId, accent);
                b.setColour (juce::TextButton::textColourOffId, text_);
                b.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff15171c));
                b.onClick = [this, i] { plug.mode->beginChangeGesture(); *plug.mode = i; plug.mode->endChangeGesture(); };
                addAndMakeVisible (b);
            }
            startTimerHz (20);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (bg);
            g.setColour (text_);
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText ("REVERB", 16, 8, 200, 26, juce::Justification::centredLeft);

            // decay preview. The time axis is fixed (square-root scale, 0 .. 20.5 s), so Pre-Delay only moves where the tail
            // starts and Decay only changes how long it is. The fill density shows Damping: dense = dark tail (highs die fast).
            auto r = graphArea.toFloat();
            g.setColour (juce::Colour (0xff0d0f13));
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (juce::Colour (0xff2a2f3a));
            g.drawRoundedRectangle (r, 6.0f, 1.0f);
            const float pre = plug.preDelay->get() * 0.001f, rt = plug.decay->get();
            const float damp = plug.damping->get() * 0.01f;
            constexpr float tMax = 20.5f;
            auto X = [&] (float t) { return r.getX() + 10.0f + std::sqrt (juce::jlimit (0.0f, tMax, t) / tMax) * (r.getWidth() - 20.0f); };
            auto Y = [&] (float db) { return r.getY() + 14.0f + (-db / 60.0f) * (r.getHeight() - 28.0f); };
            for (int db = 0; db >= -60; db -= 20)
            {
                g.setColour (juce::Colour (0xff1f2430));
                g.drawHorizontalLine ((int) Y ((float) db), r.getX() + 10.0f, r.getRight() - 10.0f);
            }
            g.setFont (juce::FontOptions (10.0f));
            for (float t : { 0.1f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f })
            {
                g.setColour (juce::Colour (0xff1f2430));
                g.drawVerticalLine ((int) X (t), r.getY() + 4.0f, r.getBottom() - 16.0f);
                g.setColour (juce::Colour (0xff5b6478));
                g.drawText (t < 1.0f ? juce::String ((int) (t * 1000)) + " ms" : juce::String ((int) t) + " s",
                            juce::Rectangle<float> (X (t) - 20.0f, r.getBottom() - 15.0f, 40.0f, 12.0f), juce::Justification::centred);
            }

            // the tail: from the end of the pre-delay it falls 60 dB over the Decay time
            juce::Path line;
            const int steps = 80;
            for (int i = 0; i <= steps; ++i)
            {
                const float t = pre + rt * (float) i / (float) steps;
                const auto pt = juce::Point<float> (X (t), Y (-60.0f * (float) i / (float) steps));
                if (i == 0) line.startNewSubPath (pt); else line.lineTo (pt);
            }
            juce::Path fill (line);
            fill.lineTo (X (pre + rt), Y (-60.0f));
            fill.lineTo (X (pre), Y (-60.0f));
            fill.closeSubPath();

            g.saveState();
            g.reduceClipRegion (fill);
            g.setColour (accent.withAlpha (0.06f + 0.34f * damp));
            g.fillPath (fill);
            // vertical hatching: the more damping, the closer the lines
            const float spacing = 14.0f - 11.0f * damp;
            g.setColour (accent.withAlpha (0.10f + 0.50f * damp));
            for (float x = X (pre); x < X (pre + rt); x += spacing)
                g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            g.restoreState();

            // pre-delay marker and the tail line
            g.setColour (juce::Colour (0xff5b6478));
            g.drawVerticalLine ((int) X (pre), Y (0.0f), Y (-60.0f));
            g.setColour (accent);
            g.strokePath (line, juce::PathStrokeType (2.5f));
            g.fillEllipse (X (pre + rt) - 4.0f, Y (-60.0f) - 4.0f, 8.0f, 8.0f);

            g.setColour (muted);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("RT60 " + juce::String (rt, 2) + " s   -   pre-delay " + juce::String ((int) std::round (pre * 1000.0f)) + " ms   -   damping " + juce::String ((int) std::round (damp * 100.0f)) + " %",
                        graphArea.reduced (10, 2), juce::Justification::topRight);
        }

        void resized() override
        {
            for (int i = 0; i < 3; ++i)
                modeBtn[i].setBounds (getWidth() - 16 - 3 * 64 + i * 64, 8, 60, 26);
            graphArea = juce::Rectangle<int> (16, 40, getWidth() - 32, 110);
            auto row = juce::Rectangle<int> (16, 164, getWidth() - 32, 150);
            const int w = row.getWidth() / 7;
            for (auto pr : { std::pair<juce::Slider*, juce::Label*> { &pdS, &pdL }, { &deS, &deL }, { &lcS, &lcL }, { &hcS, &hcL }, { &daS, &daL }, { &toS, &toL }, { &mxS, &mxL } })
            {
                auto c = row.removeFromLeft (w);
                pr.second->setBounds (c.removeFromTop (18));
                pr.first->setBounds (c);
            }
        }

    private:
        void addKnob (juce::Slider& s, juce::Label& l, const juce::String& text, juce::RangedAudioParameter& prm, std::unique_ptr<juce::SliderParameterAttachment>& att)
        {
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 84, 18);
            s.setColour (juce::Slider::rotarySliderFillColourId, accent);
            s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3a4152));
            s.setColour (juce::Slider::thumbColourId, accent);
            s.setColour (juce::Slider::textBoxTextColourId, text_);
            s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            addAndMakeVisible (s);
            l.setText (text, juce::dontSendNotification);
            l.setJustificationType (juce::Justification::centred);
            l.setColour (juce::Label::textColourId, muted);
            l.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            addAndMakeVisible (l);
            att = std::make_unique<juce::SliderParameterAttachment> (prm, s);
        }

        void timerCallback() override
        {
            for (int i = 0; i < 3; ++i)
                modeBtn[i].setToggleState (plug.mode->getIndex() == i, juce::dontSendNotification);
            repaint (graphArea);
        }

        ReverbPlugin& plug;
        juce::Slider pdS, deS, lcS, hcS, daS, toS, mxS;
        juce::Label pdL, deL, lcL, hcL, daL, toL, mxL;
        std::unique_ptr<juce::SliderParameterAttachment> pdA, deA, lcA, hcA, daA, toA, mxA;
        juce::TextButton modeBtn[3];
        juce::Rectangle<int> graphArea;
        juce::Colour bg { 0xff15171c }, accent { 0xffff9a2e }, text_ { 0xffe8eaf0 }, muted { 0xff8b93a7 };
    };

    juce::AudioProcessorEditor* ReverbPlugin::createEditor() { return new ReverbEditor (*this); }

    // ------------------------------------------------------------ 808 overdrive pedal (zero latency, see OverdriveCore.h)
    class OverdrivePlugin : public BuiltinBase
    {
    public:
        OverdrivePlugin() : BuiltinBase ("od808", "808 Overdrive", "Mid-hump overdrive pedal (zero latency)")
        {
            drive = addLinear ("dr", "Drive", 0.0f, 100.0f, 40.0f);
            tone = addLinear ("to", "Tone", 0.0f, 100.0f, 50.0f);
            on = new juce::AudioParameterBool (juce::ParameterID { "on", 1 }, "On", true);
            juce::AudioProcessor::addParameter (on);
        }

        void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buf.getNumSamples(), nch = juce::jmin (2, buf.getNumChannels());
            // The circuit keeps running while the pedal is bypassed (its filters stay warm) and the bypass is a short
            // cross-fade: switching it off and on again therefore never clicks.
            const float rampTarget = on->get() ? 1.0f : 0.0f;
            const float rampStep = (float) (1.0 / (0.012 * sr));
            if (drive->get() != lastDrive || tone->get() != lastTone)
            {
                lastDrive = drive->get();
                lastTone = tone->get();
                for (auto& c : ch)
                {
                    c.setDrive (sr, lastDrive * 0.01f);
                    c.setTone (sr, lastTone * 0.01f);
                }
            }
            const float startRamp = ramp;
            for (int c = 0; c < nch; ++c)
            {
                auto* d = buf.getWritePointer (c);
                float r = startRamp;
                for (int i = 0; i < n; ++i)
                {
                    r += juce::jlimit (-rampStep, rampStep, rampTarget - r);
                    const float x = d[i];
                    const float w = r;
                    d[i] = ch[c].process (x) * w + x * (1.0f - w);
                }
                if (c == nch - 1)
                    ramp = r;
            }
        }

        juce::AudioParameterFloat *drive, *tone;
        juce::AudioParameterBool* on;
        juce::AudioProcessorEditor* createEditor() override;

    protected:
        void prepare() override
        {
            lastDrive = lastTone = -1.0f;
            ch[0].reset(); ch[1].reset();
            ramp = on->get() ? 1.0f : 0.0f;
        }

    private:
        tcod::Channel ch[2];
        float lastDrive = -1.0f, lastTone = -1.0f, ramp = 1.0f;
    };

    class OverdriveEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit OverdriveEditor (OverdrivePlugin& p) : juce::AudioProcessorEditor (p), plug (p)
        {
            setSize (340, 480);
            addKnob (drS, drL, "DRIVE", *p.drive, drA);
            addKnob (toS, toL, "TONE", *p.tone, toA);
            drS.textFromValueFunction = [] (double v) { return juce::String ((int) std::round (v / 10.0)); };
            toS.textFromValueFunction = [] (double v) { return juce::String ((int) std::round (v / 10.0)); };
            for (auto* s : { &drS, &toS })
                s->updateText();
            startTimerHz (15);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff15171c));
            auto body = getLocalBounds().toFloat().reduced (14.0f);
            juce::ColourGradient grad (juce::Colour (0xff3fa34d), body.getX(), body.getY(), juce::Colour (0xff1f6b2c), body.getX(), body.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (body, 14.0f);
            g.setColour (juce::Colour (0xff134a1d));
            g.drawRoundedRectangle (body, 14.0f, 3.0f);

            // screws
            for (auto pt : { juce::Point<float> (body.getX() + 18, body.getY() + 18), { body.getRight() - 18, body.getY() + 18 },
                             { body.getX() + 18, body.getBottom() - 18 }, { body.getRight() - 18, body.getBottom() - 18 } })
            {
                g.setColour (juce::Colour (0xffb9c4b9));
                g.fillEllipse (pt.x - 5, pt.y - 5, 10, 10);
                g.setColour (juce::Colour (0xff4d5a4d));
                g.drawLine (pt.x - 3, pt.y - 3, pt.x + 3, pt.y + 3, 1.5f);
            }

            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
            g.drawText ("808", juce::Rectangle<int> (0, 292, getWidth(), 40), juce::Justification::centred);
            g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            g.drawText ("OVERDRIVE", juce::Rectangle<int> (0, 326, getWidth(), 20), juce::Justification::centred);

            // LED and foot switch (the pedal's own switch: a short cross-fade, never clicks)
            const bool o = plug.on->get();
            g.setColour (o ? juce::Colour (0xffff3b30) : juce::Colour (0xff4a2a2a));
            g.fillEllipse (getWidth() * 0.5f - 7.0f, 368.0f, 14.0f, 14.0f);
            if (o)
            {
                g.setColour (juce::Colour (0x40ff3b30));
                g.fillEllipse (getWidth() * 0.5f - 14.0f, 361.0f, 28.0f, 28.0f);
            }
            auto fs = footSwitch().toFloat();
            juce::ColourGradient metal (juce::Colour (0xffe6e9ea), fs.getX(), fs.getY(), juce::Colour (0xff8d9496), fs.getRight(), fs.getBottom(), false);
            g.setGradientFill (metal);
            g.fillEllipse (fs);
            g.setColour (juce::Colour (0xff3b4244));
            g.drawEllipse (fs, 2.5f);
            g.setColour (juce::Colour (0xff6b7476));
            g.drawEllipse (fs.reduced (14.0f), 1.5f);
            g.setColour (juce::Colour (0xffd7e8d7));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (o ? "ON  -  click to bypass" : "BYPASSED  -  click to turn on", juce::Rectangle<int> (0, 452, getWidth(), 14), juce::Justification::centred);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (footSwitch().toFloat().contains (e.position))
            {
                plug.on->beginChangeGesture();
                *plug.on = ! plug.on->get();
                plug.on->endChangeGesture();
                repaint();
            }
        }

        void resized() override
        {
            const int w = (getWidth() - 56) / 2;
            juce::Slider* sl[2] = { &drS, &toS };
            juce::Label* lb[2] = { &drL, &toL };
            for (int i = 0; i < 2; ++i)
            {
                auto c = juce::Rectangle<int> (28 + i * w, 36, w, 232);
                lb[i]->setBounds (c.removeFromTop (22));
                sl[i]->setBounds (c.removeFromTop (c.getHeight()).withTrimmedBottom (4));
            }
        }

    private:
        juce::Rectangle<int> footSwitch() const { return juce::Rectangle<int> (getWidth() / 2 - 40, 388, 80, 60); }

        void addKnob (juce::Slider& s, juce::Label& l, const juce::String& text, juce::RangedAudioParameter& prm, std::unique_ptr<juce::SliderParameterAttachment>& att)
        {
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 20);
            s.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xfff2f4ee));
            s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff134a1d));
            s.setColour (juce::Slider::thumbColourId, juce::Colour (0xfff2f4ee));
            s.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
            s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            addAndMakeVisible (s);
            l.setText (text, juce::dontSendNotification);
            l.setJustificationType (juce::Justification::centred);
            l.setColour (juce::Label::textColourId, juce::Colours::white);
            l.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            addAndMakeVisible (l);
            att = std::make_unique<juce::SliderParameterAttachment> (prm, s);
        }

        void timerCallback() override { repaint (0, 360, getWidth(), 120); }

        OverdrivePlugin& plug;
        juce::Slider drS, toS;
        juce::Label drL, toL;
        std::unique_ptr<juce::SliderParameterAttachment> drA, toA;
    };

    juce::AudioProcessorEditor* OverdrivePlugin::createEditor() { return new OverdriveEditor (*this); }

    // ------------------------------------------------------------ guitar amp + pedal board (zero latency, see AmpBoard.h)
    class AmpPlugin;
    std::vector<AmpPlugin*>& ampRegistry() { static std::vector<AmpPlugin*> r; return r; }
    juce::CriticalSection& ampRegistryLock() { static juce::CriticalSection l; return l; }

    class AmpPlugin : public BuiltinBase, private juce::AudioProcessorParameter::Listener, private juce::AsyncUpdater
    {
    public:
        static constexpr int kScenes = 4;
        static constexpr int kSmooth = 29;

        AmpPlugin() : BuiltinBase ("amp", "Amp", "Guitar amplifier, cabinet and pedals with four scenes (zero latency)")
        {
            // ---- amplifier
            model = addChoice ("md", "Amp", { "TWN Clean", "RLD Clean", "MRS Crunch", "VX Crunch", "PVY Dist", "MB Dist" }, 2);
            cab = addChoice ("cb", "Cabinet", { "Off", "1x12", "2x12", "4x12" }, 3);
            input = addLinear ("in", "Input", -12.0f, 12.0f, 0.0f);
            gain = addLinear ("ga", "Gain", 0.0f, 100.0f, 50.0f);
            bass = addLinear ("ba", "Bass", 0.0f, 100.0f, 50.0f);
            mid = addLinear ("mi", "Mid", 0.0f, 100.0f, 50.0f);
            treble = addLinear ("tr", "Treble", 0.0f, 100.0f, 50.0f);
            presence = addLinear ("pr", "Presence", 0.0f, 100.0f, 50.0f);
            master = addLinear ("ma", "Master", -40.0f, 6.0f, -6.0f);

            // ---- pedals, in the order of the signal path
            driveOn = addSwitch ("dro", "Drive on");
            driveAmt = addLinear ("drd", "Drive", 0.0f, 100.0f, 50.0f);
            driveTone = addLinear ("drt", "Drive tone", 0.0f, 100.0f, 50.0f);
            driveLevel = addLinear ("drl", "Drive level", -12.0f, 12.0f, 0.0f);

            compOn = addSwitch ("cpo", "Comp on");
            compSens = addLinear ("cps", "Comp sensitivity", 0.0f, 100.0f, 50.0f);
            compLevel = addLinear ("cpl", "Comp level", -12.0f, 12.0f, 0.0f);

            eqOn = addSwitch ("eqo", "EQ on");
            static const char* eqIds[8] = { "e0", "e1", "e2", "e3", "e4", "e5", "e6", "e7" };
            static const char* eqNames[8] = { "EQ 62", "EQ 125", "EQ 250", "EQ 500", "EQ 1k", "EQ 2k", "EQ 4k", "EQ 8k" };
            for (int i = 0; i < 8; ++i)
                eqBand[i] = addLinear (eqIds[i], eqNames[i], -12.0f, 12.0f, 0.0f);

            chorusOn = addSwitch ("cho", "Chorus on");
            chorusRate = addLinear ("chr", "Chorus rate", 0.1f, 8.0f, 0.8f);
            chorusDepth = addLinear ("chd", "Chorus depth", 0.0f, 100.0f, 50.0f);
            chorusMix = addLinear ("chm", "Chorus mix", 0.0f, 100.0f, 50.0f);

            delayOn = addSwitch ("dlo", "Delay on");
            std::vector<juce::String> names;
            for (int i = 0; i < tcfx::kDivisions; ++i)
                names.push_back (tcfx::divisions()[i].name);
            delayDiv = addChoice ("dlt", "Delay time", names, 5);
            delayFb = addLinear ("dlf", "Delay feedback", 0.0f, 95.0f, 35.0f);
            delayMix = addLinear ("dlm", "Delay mix", 0.0f, 100.0f, 30.0f);
            delayTone = addLinear ("dlk", "Delay tone", 0.0f, 100.0f, 60.0f);

            reverbOn = addSwitch ("rvo", "Reverb on");
            reverbType = addChoice ("rvt", "Reverb type", { "Hall", "Plate", "Room" }, 0);
            reverbDecay = addLinear ("rvd", "Reverb decay", 0.3f, 8.0f, 2.2f);
            reverbMix = addLinear ("rvm", "Reverb mix", 0.0f, 100.0f, 25.0f);
            reverbTone = addLinear ("rvk", "Reverb tone", 0.0f, 100.0f, 50.0f);

            loopP = addSwitch ("lp", "FX loop");

            // ---- not part of a scene: who plays this amp, and which scene is active
            guitarist = addChoice ("gt", "Guitarist", { "1", "2", "3", "4" }, 0);
            sceneP = new juce::AudioParameterInt (juce::ParameterID { "sc", 1 }, "Scene", 0, kScenes - 1, 0);
            juce::AudioProcessor::addParameter (sceneP);
            sceneParamCount = getParameters().size();   // everything added so far, except the last two, belongs to a scene

            smP = { input, gain, bass, mid, treble, presence, master, driveAmt, driveTone, driveLevel, compSens, compLevel,
                    eqBand[0], eqBand[1], eqBand[2], eqBand[3], eqBand[4], eqBand[5], eqBand[6], eqBand[7],
                    chorusRate, chorusDepth, chorusMix, delayFb, delayMix, delayTone, reverbDecay, reverbMix, reverbTone };

            for (auto* p : getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                {
                    if (rp == guitarist || rp == sceneP)
                    {
                        sceneIndex.push_back (-1);
                        continue;
                    }
                    sceneIndex.push_back ((int) sceneParams.size());
                    sceneParams.push_back (rp);
                }
            for (auto& s : sceneVal)
            {
                s.resize (sceneParams.size());
                for (size_t k = 0; k < sceneParams.size(); ++k)
                    s[k] = sceneParams[k]->getDefaultValue();
            }
            buildDefaultScenes();
            for (auto* p : getParameters())
                p->addListener (this);

            guitarName = "Гитарист 1"_ru;
            {
                const juce::ScopedLock sl (ampRegistryLock());
                ampRegistry().push_back (this);
            }
            loadScene (0);
        }

        ~AmpPlugin() override
        {
            cancelPendingUpdate();
            for (auto* p : getParameters())
                p->removeListener (this);
            const juce::ScopedLock sl (ampRegistryLock());
            auto& r = ampRegistry();
            r.erase (std::remove (r.begin(), r.end(), this), r.end());
        }

        // ------------------------------------------------------------ scenes
        void setScene (int n)
        {
            n = juce::jlimit (0, kScenes - 1, n);
            sceneP->beginChangeGesture();
            *sceneP = n;
            sceneP->endChangeGesture();
            loadScene (n);
        }
        int activeScene() const { return sceneP->get(); }
        juce::String getSceneName (int i) const { return sceneName[juce::jlimit (0, kScenes - 1, i)]; }
        void setSceneName (int i, const juce::String& n) { sceneName[juce::jlimit (0, kScenes - 1, i)] = n.trim().isEmpty() ? juce::String (i + 1) : n.trim(); }
        int guitaristSlot() const { return guitarist->getIndex(); }
        juce::String guitaristName() const { return guitarName; }
        void setGuitaristName (const juce::String& n) { guitarName = n.trim().isEmpty() ? juce::String ("Guitarist") : n.trim(); }

        void getStateInformation (juce::MemoryBlock& dest) override
        {
            juce::XmlElement xml ("TCFX");
            for (auto* p : getParameters())
                if (auto* pp = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                    xml.setAttribute (pp->paramID, (double) pp->getValue());
            xml.setAttribute ("guitarName", guitarName);
            auto* sc = xml.createNewChildElement ("SCENES");
            for (int i = 0; i < kScenes; ++i)
            {
                auto* e = sc->createNewChildElement ("S");
                e->setAttribute ("name", sceneName[i]);
                juce::StringArray v;
                for (float f : sceneVal[i])
                    v.add (juce::String (f, 5));
                e->setAttribute ("v", v.joinIntoString (" "));
            }
            copyXmlToBinary (xml, dest);
        }

        void setStateInformation (const void* data, int size) override
        {
            auto xml = getXmlFromBinary (data, size);
            if (xml == nullptr)
                return;
            loadingState = true;
            if (xml->hasAttribute ("guitarName"))
                guitarName = xml->getStringAttribute ("guitarName");
            bool haveScenes = false;
            if (auto* sc = xml->getChildByName ("SCENES"))
            {
                int i = 0;
                for (auto* e : sc->getChildWithTagNameIterator ("S"))
                {
                    if (i >= kScenes)
                        break;
                    sceneName[i] = e->getStringAttribute ("name", juce::String (i + 1));
                    const auto v = juce::StringArray::fromTokens (e->getStringAttribute ("v"), " ", "");
                    if ((size_t) v.size() == sceneParams.size())
                    {
                        for (int k = 0; k < v.size(); ++k)
                            sceneVal[i][(size_t) k] = v[k].getFloatValue();
                        haveScenes = true;
                    }
                    ++i;
                }
            }
            for (auto* p : getParameters())
                if (auto* pp = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                    if (xml->hasAttribute (pp->paramID))
                        pp->setValueNotifyingHost ((float) xml->getDoubleAttribute (pp->paramID));
            if (! haveScenes)   // a project from before the scenes: what is loaded becomes the active scene
                storeCurrent (sceneP->get());
            loadingState = false;
        }

        // ------------------------------------------------------------ audio
        void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buf.getNumSamples(), nch = juce::jmin (2, buf.getNumChannels());
            if (nch == 0 || n == 0)
                return;

            double bpm = 120.0;
            if (auto* ph = getPlayHead())
                if (auto pos = ph->getPosition())
                    if (auto b = pos->getBpm())
                        bpm = *b;

            // continuous values follow their targets smoothly (a scene change or a moved knob is not a step)
            const float k = firstBlock ? 1.0f : 1.0f - std::exp (-(float) n / (0.025f * (float) sr));
            for (int i = 0; i < kSmooth; ++i)
            {
                const float t = smP[(size_t) i]->get();
                sm[(size_t) i] = firstBlock ? t : sm[(size_t) i] + (t - sm[(size_t) i]) * k;
            }
            firstBlock = false;

            // the amp model, the cabinet and the loop position cannot be blended: the sound is faded out for one block,
            // changed, and faded in for the next one
            const int dModel = model->getIndex(), dCab = cab->getIndex();
            const bool dLoop = loopP->get();
            if (! haveApplied)
            {
                aModel = dModel; aCab = dCab; aLoop = dLoop;
                haveApplied = true;
            }
            if (duckPhase == 0 && (aModel != dModel || aCab != dCab || aLoop != dLoop))
                duckPhase = 1;
            if (duckPhase == 1 && duckGain <= 0.0f)
            {
                aModel = dModel; aCab = dCab; aLoop = dLoop;
                board.resetAmp();   // silent now: the old model's filter and clipper states must not leak into the new one
                duckPhase = 2;
            }

            tcamp::Params ap;
            ap.model = aModel;
            ap.cab = aCab;
            ap.inputDb = sm[0];
            ap.gain = sm[1] * 0.01f;
            ap.bass = sm[2] * 0.01f;
            ap.mid = sm[3] * 0.01f;
            ap.treble = sm[4] * 0.01f;
            ap.presence = sm[5] * 0.01f;
            ap.masterDb = sm[6];
            board.setAmp (ap);

            tcboard::Pedals pd;
            pd.driveOn = driveOn->get(); pd.compOn = compOn->get(); pd.eqOn = eqOn->get();
            pd.chorusOn = chorusOn->get(); pd.delayOn = delayOn->get(); pd.reverbOn = reverbOn->get();
            pd.loop = aLoop;
            pd.drive = sm[7] * 0.01f; pd.driveTone = sm[8] * 0.01f; pd.driveLevelDb = sm[9];
            pd.compSens = sm[10] * 0.01f; pd.compLevelDb = sm[11];
            for (int i = 0; i < 8; ++i)
                pd.eqDb[i] = sm[(size_t) (12 + i)];
            pd.chorusRate = sm[20]; pd.chorusDepth = sm[21] * 0.01f; pd.chorusMix = sm[22] * 0.01f;
            pd.delayDiv = delayDiv->getIndex();
            pd.delayFeedback = sm[23] * 0.01f; pd.delayMix = sm[24] * 0.01f; pd.delayTone = sm[25] * 0.01f;
            pd.reverbType = reverbType->getIndex();
            pd.reverbDecay = sm[26]; pd.reverbMix = sm[27] * 0.01f; pd.reverbTone = sm[28] * 0.01f;

            board.process (buf.getWritePointer (0), nch > 1 ? buf.getWritePointer (1) : nullptr, n, pd, bpm);

            if (duckPhase != 0)
            {
                const float step = (float) (1.0 / (0.020 * sr));
                const float start = duckGain;
                for (int c = 0; c < nch; ++c)
                {
                    auto* d = buf.getWritePointer (c);
                    float g = start;
                    for (int i = 0; i < n; ++i)
                    {
                        if (duckPhase == 1)
                            g = std::max (0.0f, g - step);
                        else
                            g = std::min (1.0f, g + step);
                        d[i] *= g;
                    }
                    if (c == nch - 1)
                        duckGain = g;
                }
                if (duckPhase == 2 && duckGain >= 1.0f)
                    duckPhase = 0;
            }
        }

        juce::AudioParameterChoice *model, *cab, *delayDiv, *reverbType, *guitarist;
        juce::AudioParameterInt* sceneP;
        juce::AudioParameterBool *driveOn, *compOn, *eqOn, *chorusOn, *delayOn, *reverbOn, *loopP;
        juce::AudioParameterFloat *input, *gain, *bass, *mid, *treble, *presence, *master;
        juce::AudioParameterFloat *driveAmt, *driveTone, *driveLevel, *compSens, *compLevel, *eqBand[8];
        juce::AudioParameterFloat *chorusRate, *chorusDepth, *chorusMix, *delayFb, *delayMix, *delayTone;
        juce::AudioParameterFloat *reverbDecay, *reverbMix, *reverbTone;
        juce::AudioProcessorEditor* createEditor() override;

    protected:
        void prepare() override
        {
            board.prepare (sr);
            firstBlock = true;
            haveApplied = false;
            duckPhase = 0;
            duckGain = 1.0f;
        }

    private:
        juce::AudioParameterChoice* addChoice (const char* pid, const juce::String& label, std::vector<juce::String> items, int def)
        {
            juce::StringArray a;
            for (auto& s : items)
                a.add (s);
            auto* p = new juce::AudioParameterChoice (juce::ParameterID { pid, 1 }, label, a, def);
            juce::AudioProcessor::addParameter (p);
            return p;
        }
        juce::AudioParameterBool* addSwitch (const char* pid, const juce::String& label)
        {
            auto* p = new juce::AudioParameterBool (juce::ParameterID { pid, 1 }, label, false);
            juce::AudioProcessor::addParameter (p);
            return p;
        }

        // the parameters of a scene by id: real values are turned into the 0..1 values that the scenes keep
        void put (int scene, juce::RangedAudioParameter* p, float real)
        {
            for (size_t k = 0; k < sceneParams.size(); ++k)
                if (sceneParams[k] == p)
                    sceneVal[scene][k] = p->convertTo0to1 (real);
        }

        void buildDefaultScenes()
        {
            sceneName[0] = "Clean"; sceneName[1] = "Crunch"; sceneName[2] = "Solo"; sceneName[3] = "Ambient";

            // 1 Clean: bright clean amp with a little room
            put (0, model, 0); put (0, cab, 2); put (0, gain, 35); put (0, bass, 55); put (0, mid, 45); put (0, treble, 60);
            put (0, reverbOn, 1); put (0, reverbMix, 18); put (0, reverbDecay, 1.8f);

            // 2 Crunch: British crunch
            put (1, model, 2); put (1, cab, 3); put (1, gain, 55); put (1, bass, 55); put (1, mid, 60); put (1, treble, 55);

            // 3 Solo: high gain, drive in front of it, delay and reverb
            put (2, model, 4); put (2, cab, 3); put (2, gain, 65); put (2, mid, 60);
            put (2, driveOn, 1); put (2, driveAmt, 30); put (2, driveTone, 60);
            put (2, delayOn, 1); put (2, delayDiv, 6); put (2, delayFb, 35); put (2, delayMix, 28);
            put (2, reverbOn, 1); put (2, reverbMix, 20);

            // 4 Ambient: clean with chorus, a long delay and a big hall
            put (3, model, 1); put (3, cab, 2); put (3, gain, 30);
            put (3, chorusOn, 1); put (3, chorusRate, 0.6f); put (3, chorusDepth, 55); put (3, chorusMix, 45);
            put (3, delayOn, 1); put (3, delayDiv, 9); put (3, delayFb, 50); put (3, delayMix, 40);
            put (3, reverbOn, 1); put (3, reverbDecay, 4.5f); put (3, reverbMix, 35);
        }

        void storeCurrent (int scene)
        {
            for (size_t k = 0; k < sceneParams.size(); ++k)
                sceneVal[juce::jlimit (0, kScenes - 1, scene)][k] = sceneParams[k]->getValue();
        }

        void loadScene (int n)
        {
            n = juce::jlimit (0, kScenes - 1, n);
            loadingScene = true;
            active = n;
            for (size_t k = 0; k < sceneParams.size(); ++k)
                sceneParams[k]->setValueNotifyingHost (sceneVal[n][k]);
            loadingScene = false;
        }

        // every edit of a parameter is kept in the active scene; a change of the scene parameter (editor, automation) loads it
        void parameterValueChanged (int index, float) override
        {
            if (loadingScene || loadingState)
                return;
            if (index >= 0 && index < (int) sceneIndex.size() && sceneIndex[(size_t) index] >= 0)
            {
                const auto k = (size_t) sceneIndex[(size_t) index];
                sceneVal[(size_t) active][k] = sceneParams[k]->getValue();
            }
            else if (index >= 0 && getParameters()[index] == sceneP && sceneP->get() != active)
                triggerAsyncUpdate();
        }
        void parameterGestureChanged (int, bool) override {}
        void handleAsyncUpdate() override
        {
            if (sceneP->get() != active)
                loadScene (sceneP->get());
        }

        tcboard::Board board;
        std::array<juce::AudioParameterFloat*, kSmooth> smP {};
        std::array<float, kSmooth> sm {};
        std::vector<juce::RangedAudioParameter*> sceneParams;
        std::vector<int> sceneIndex;
        std::vector<float> sceneVal[kScenes];
        juce::String sceneName[kScenes];
        juce::String guitarName;
        float duckGain = 1.0f;
        int sceneParamCount = 0, active = 0, duckPhase = 0, aModel = 0, aCab = 0;
        bool aLoop = false, haveApplied = false, firstBlock = true;
        std::atomic<bool> loadingScene { false }, loadingState { false };
    };

    class AmpEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit AmpEditor (AmpPlugin& p) : juce::AudioProcessorEditor (p), plug (p)
        {
            setSize (1340, 660);

            // ---- header: guitarist and the four scenes
            guitarBtn.onClick = [this] { showGuitaristMenu(); };
            styleButton (guitarBtn);
            addAndMakeVisible (guitarBtn);
            for (int i = 0; i < AmpPlugin::kScenes; ++i)
            {
                auto& b = sceneBtn[i];
                styleButton (b);
                b.onClick = [this, i] { plug.setScene (i); };
                b.onRename = [this, i]
                {
                    askName ("Scene " + juce::String (i + 1), plug.getSceneName (i), [this, i] (const juce::String& t) { plug.setSceneName (i, t); refresh(); });
                };
                b.setTooltip ("Click: switch to this scene. Right click: rename. Every change of a knob is kept in the active scene.");
                addAndMakeVisible (b);
            }

            // ---- amplifier
            const char* modelNames[6] = { "TWN Clean", "RLD Clean", "MRS Crunch", "VX Crunch", "PVY Dist", "MB Dist" };
            for (int i = 0; i < 6; ++i)
            {
                styleButton (modelBtn[i]);
                modelBtn[i].setButtonText (modelNames[i]);
                modelBtn[i].onClick = [this, i] { setChoice (*plug.model, i); };
                addAndMakeVisible (modelBtn[i]);
            }
            const char* cabNames[4] = { "NO CAB", "1x12", "2x12", "4x12" };
            for (int i = 0; i < 4; ++i)
            {
                styleButton (cabBtn[i]);
                cabBtn[i].setButtonText (cabNames[i]);
                cabBtn[i].onClick = [this, i] { setChoice (*plug.cab, i); };
                addAndMakeVisible (cabBtn[i]);
            }
            styleButton (loopBtn);
            loopBtn.onClick = [this] { plug.loopP->beginChangeGesture(); *plug.loopP = ! plug.loopP->get(); plug.loopP->endChangeGesture(); };
            loopBtn.setTooltip ("Where chorus, delay and reverb sit: in front of the amp, or in its effects loop (after the pre-amp, before the power amp). "
                                "The loop is the right place when the amp itself distorts.");
            addAndMakeVisible (loopBtn);

            auto tenth = [] (double v) { return juce::String (v / 10.0, 1); };
            auto dB = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; };
            auto hz = [] (double v) { return juce::String (v, 1) + " Hz"; };
            auto sec = [] (double v) { return juce::String (v, 1) + " s"; };

            ampKnobs = { addKnob ("INPUT", *plug.input, dB), addKnob ("GAIN", *plug.gain, tenth), addKnob ("BASS", *plug.bass, tenth),
                         addKnob ("MID", *plug.mid, tenth), addKnob ("TREBLE", *plug.treble, tenth), addKnob ("PRESENCE", *plug.presence, tenth),
                         addKnob ("MASTER", *plug.master, dB) };

            // ---- pedals, in the order of the signal path
            setupPedal (pedals[0], "DRIVE", *plug.driveOn, { addKnob ("DRIVE", *plug.driveAmt, tenth), addKnob ("TONE", *plug.driveTone, tenth),
                                                              addKnob ("LEVEL", *plug.driveLevel, dB) });
            setupPedal (pedals[1], "COMP", *plug.compOn, { addKnob ("SUSTAIN", *plug.compSens, tenth), addKnob ("LEVEL", *plug.compLevel, dB) });
            setupPedal (pedals[2], "EQ", *plug.eqOn, {});
            setupPedal (pedals[3], "CHORUS", *plug.chorusOn, { addKnob ("RATE", *plug.chorusRate, hz), addKnob ("DEPTH", *plug.chorusDepth, tenth),
                                                                addKnob ("MIX", *plug.chorusMix, tenth) });
            setupPedal (pedals[4], "DELAY", *plug.delayOn, { addKnob ("FEEDBACK", *plug.delayFb, tenth), addKnob ("MIX", *plug.delayMix, tenth),
                                                              addKnob ("TONE", *plug.delayTone, tenth) });
            setupPedal (pedals[5], "REVERB", *plug.reverbOn, { addKnob ("DECAY", *plug.reverbDecay, sec), addKnob ("MIX", *plug.reverbMix, tenth),
                                                                addKnob ("TONE", *plug.reverbTone, tenth) });

            // EQ: eight faders
            static const char* eqLabels[8] = { "62", "125", "250", "500", "1k", "2k", "4k", "8k" };
            for (int i = 0; i < 8; ++i)
            {
                auto& s = eqS[i];
                s.setSliderStyle (juce::Slider::LinearVertical);
                s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 40, 16);
                s.setNumDecimalPlacesToDisplay (0);
                s.setColour (juce::Slider::thumbColourId, accent);
                s.setColour (juce::Slider::trackColourId, accent.withAlpha (0.5f));
                s.setColour (juce::Slider::textBoxTextColourId, text_);
                s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
                s.setDoubleClickReturnValue (true, 0.0);
                addAndMakeVisible (s);
                eqA[i] = std::make_unique<juce::SliderParameterAttachment> (*plug.eqBand[i], s);
                eqL[i].setText (eqLabels[i], juce::dontSendNotification);
                eqL[i].setJustificationType (juce::Justification::centred);
                eqL[i].setColour (juce::Label::textColourId, muted);
                eqL[i].setFont (juce::FontOptions (11.0f, juce::Font::bold));
                addAndMakeVisible (eqL[i]);
            }

            // delay time and reverb type
            for (int i = 0; i < tcfx::kDivisions; ++i)
                delayTime.addItem (tcfx::divisions()[i].name, i + 1);
            delayTime.setTooltip ("Delay time as a note value of the project tempo");
            addAndMakeVisible (delayTime);
            delayTimeA = std::make_unique<juce::ComboBoxParameterAttachment> (*plug.delayDiv, delayTime);
            reverbTypeBox.addItem ("Hall", 1);
            reverbTypeBox.addItem ("Plate", 2);
            reverbTypeBox.addItem ("Room", 3);
            addAndMakeVisible (reverbTypeBox);
            reverbTypeA = std::make_unique<juce::ComboBoxParameterAttachment> (*plug.reverbType, reverbTypeBox);

            resized();
            startTimerHz (15);
            refresh();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (bg);
            auto body = getLocalBounds().toFloat().reduced (8.0f);
            juce::ColourGradient grad (juce::Colour (0xff23262e), body.getX(), body.getY(), juce::Colour (0xff181a20), body.getX(), body.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (body, 10.0f);
            g.setColour (juce::Colour (0xff3a4152));
            g.drawRoundedRectangle (body, 10.0f, 2.0f);

            g.setColour (accent);
            g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
            g.drawText ("AMP", 24, 14, 80, 34, juce::Justification::centredLeft);

            g.setColour (muted);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("CABINET", cabBtn[0].getX(), cabBtn[0].getY() - 13, 70, 12, juce::Justification::centredLeft);
            g.drawText ("AMPLIFIER", modelBtn[0].getX(), modelBtn[0].getY() - 13, 90, 12, juce::Justification::centredLeft);

            for (int i = 0; i < 6; ++i)
            {
                auto r = pedals[(size_t) i].area.toFloat();
                const bool on = pedals[(size_t) i].sw.getToggleState();
                g.setColour (juce::Colour (0xff1c1f27));
                g.fillRoundedRectangle (r, 8.0f);
                g.setColour (on ? accent.withAlpha (0.85f) : juce::Colour (0xff3a4152));
                g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, on ? 2.0f : 1.2f);
                g.setColour (on ? accent : muted);
                g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                g.drawText (pedals[(size_t) i].title, r.getX() + 12, r.getY() + 8, r.getWidth() - 24, 18, juce::Justification::centredLeft);
                // the arrow of the signal path between the pedals
                if (i < 5)
                {
                    g.setColour (muted);
                    const float x = r.getRight() + 5.0f, y = r.getCentreY();
                    g.drawArrow (juce::Line<float> (x - 4.0f, y, x + 5.0f, y), 1.5f, 6.0f, 6.0f);
                }
            }
            // the loop bracket over the three time effects
            g.setColour (muted.withAlpha (0.6f));
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            const auto a = pedals[3].area, b = pedals[5].area;
            g.drawText (plug.loopP->get() ? "IN THE EFFECTS LOOP" : "IN FRONT OF THE AMP", a.getX(), a.getY() - 16, b.getRight() - a.getX(), 12, juce::Justification::centred);
        }

        void resized() override
        {
            if (ampKnobs.size() < 7)   // setSize() in the constructor calls this before the knobs exist
                return;
            guitarBtn.setBounds (112, 16, 200, 32);
            for (int i = 0; i < AmpPlugin::kScenes; ++i)
                sceneBtn[i].setBounds (336 + i * 158, 14, 150, 36);

            for (int i = 0; i < 6; ++i)
                modelBtn[i].setBounds (24 + i * 104, 82, 98, 28);
            for (int i = 0; i < 4; ++i)
                cabBtn[i].setBounds (676 + i * 70, 82, 64, 28);
            loopBtn.setBounds (getWidth() - 24 - 250, 82, 250, 28);

            const int kw = (getWidth() - 48) / 7;
            for (int i = 0; i < 7; ++i)
                placeKnob (ampKnobs[(size_t) i], { 24 + i * kw, 124, kw, 140 });

            // pedal board
            static const int widths[6] = { 200, 140, 300, 200, 200, 200 };
            int x = 24;
            for (int i = 0; i < 6; ++i)
            {
                auto& pd = pedals[(size_t) i];
                pd.area = { x, 308, widths[i], 332 };
                pd.sw.setBounds (pd.area.getX() + 16, pd.area.getBottom() - 62, pd.area.getWidth() - 32, 46);
                x += widths[i] + 12;
                auto inner = pd.area.reduced (8, 0).withTrimmedTop (36).withTrimmedBottom (72);
                if (i == 4)
                    delayTime.setBounds (inner.removeFromTop (30).reduced (8, 0));
                if (i == 5)
                    reverbTypeBox.setBounds (inner.removeFromTop (30).reduced (8, 0));
                if (! pd.knobs.empty())
                {
                    const int w = inner.getWidth() / (int) pd.knobs.size();
                    for (size_t k = 0; k < pd.knobs.size(); ++k)
                        placeKnob (pd.knobs[k], inner.removeFromLeft (w).withTrimmedTop (12));
                }
            }
            auto eq = pedals[2].area.reduced (10, 0).withTrimmedTop (40).withTrimmedBottom (78);
            const int ew = eq.getWidth() / 8;
            for (int i = 0; i < 8; ++i)
            {
                auto c = eq.removeFromLeft (ew);
                eqL[i].setBounds (c.removeFromBottom (16));
                eqS[i].setBounds (c);
            }
        }

    private:
        // ---- little helpers
        struct Knob
        {
            juce::Slider s;
            juce::Label l;
            std::unique_ptr<juce::SliderParameterAttachment> a;
        };
        struct Pedal
        {
            juce::String title;
            juce::Rectangle<int> area;
            juce::TextButton sw;
            std::unique_ptr<juce::ButtonParameterAttachment> swA;
            std::vector<Knob*> knobs;
        };
        struct SceneButton : juce::TextButton
        {
            std::function<void()> onRename;
            void mouseDown (const juce::MouseEvent& e) override
            {
                if (e.mods.isPopupMenu())
                {
                    if (onRename)
                        onRename();
                    return;
                }
                juce::TextButton::mouseDown (e);
            }
        };

        void styleButton (juce::TextButton& b)
        {
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff262b36));
            b.setColour (juce::TextButton::buttonOnColourId, accent);
            b.setColour (juce::TextButton::textColourOffId, text_);
            b.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff15171c));
        }

        Knob* addKnob (const juce::String& text, juce::RangedAudioParameter& prm, std::function<juce::String (double)> fmt)
        {
            knobs.push_back (std::make_unique<Knob>());
            auto* k = knobs.back().get();
            auto& s = k->s;
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 18);
            s.setColour (juce::Slider::rotarySliderFillColourId, accent);
            s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3a4152));
            s.setColour (juce::Slider::thumbColourId, accent);
            s.setColour (juce::Slider::textBoxTextColourId, text_);
            s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            addAndMakeVisible (s);
            k->l.setText (text, juce::dontSendNotification);
            k->l.setJustificationType (juce::Justification::centred);
            k->l.setColour (juce::Label::textColourId, muted);
            k->l.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            addAndMakeVisible (k->l);
            k->a = std::make_unique<juce::SliderParameterAttachment> (prm, s);
            if (fmt)
            {
                s.textFromValueFunction = fmt;
                s.updateText();
            }
            return k;
        }

        static void placeKnob (Knob* k, juce::Rectangle<int> r)
        {
            k->l.setBounds (r.removeFromTop (16));
            k->s.setBounds (r);
        }

        void setupPedal (Pedal& p, const juce::String& title, juce::AudioParameterBool& on, std::vector<Knob*> ks)
        {
            p.title = title;
            p.knobs = std::move (ks);
            styleButton (p.sw);
            p.sw.setClickingTogglesState (true);
            p.sw.setButtonText ("ON / OFF");
            p.swA = std::make_unique<juce::ButtonParameterAttachment> (on, p.sw);
            p.sw.onStateChange = [this] { repaint(); };
            addAndMakeVisible (p.sw);
        }

        static void setChoice (juce::AudioParameterChoice& p, int i)
        {
            p.beginChangeGesture();
            p = i;
            p.endChangeGesture();
        }

        void askName (const juce::String& title, const juce::String& initial, std::function<void (juce::String)> done)
        {
            auto* w = new juce::AlertWindow (title, "Name:", juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("t", initial);
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([w, done] (int r)
                                {
                                    if (r == 1 && done)
                                        done (w->getTextEditorContents ("t").trim());
                                }),
                                true);
        }

        void showGuitaristMenu()
        {
            juce::PopupMenu m;
            for (int i = 0; i < 4; ++i)
                m.addItem (i + 1, "Guitarist slot " + juce::String (i + 1) + " (phone tab)", true, plug.guitaristSlot() == i);
            m.addSeparator();
            m.addItem (10, "Rename...");
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&guitarBtn), [this] (int r)
            {
                if (r >= 1 && r <= 4)
                    setChoice (*plug.guitarist, r - 1);
                else if (r == 10)
                    askName ("Guitarist", plug.guitaristName(), [this] (const juce::String& t) { plug.setGuitaristName (t); refresh(); });
                refresh();
            });
        }

        void refresh()
        {
            guitarBtn.setButtonText (plug.guitaristName() + "  [" + juce::String (plug.guitaristSlot() + 1) + "]");
            guitarBtn.setTooltip ("Which guitarist plays through this amp. On the phone he picks his tab and switches his four scenes.");
            for (int i = 0; i < AmpPlugin::kScenes; ++i)
            {
                sceneBtn[i].setButtonText (juce::String (i + 1) + "  " + plug.getSceneName (i));
                sceneBtn[i].setToggleState (plug.activeScene() == i, juce::dontSendNotification);
            }
            for (int i = 0; i < 6; ++i)
                modelBtn[i].setToggleState (plug.model->getIndex() == i, juce::dontSendNotification);
            for (int i = 0; i < 4; ++i)
                cabBtn[i].setToggleState (plug.cab->getIndex() == i, juce::dontSendNotification);
            loopBtn.setButtonText (plug.loopP->get() ? "TIME FX: IN THE EFFECTS LOOP" : "TIME FX: IN FRONT OF THE AMP");
            loopBtn.setToggleState (plug.loopP->get(), juce::dontSendNotification);
        }

        void timerCallback() override
        {
            refresh();
            repaint();
        }

        AmpPlugin& plug;
        SceneButton sceneBtn[AmpPlugin::kScenes];
        juce::TextButton guitarBtn, modelBtn[6], cabBtn[4], loopBtn;
        std::vector<std::unique_ptr<Knob>> knobs;
        std::vector<Knob*> ampKnobs;
        Pedal pedals[6];
        juce::Slider eqS[8];
        juce::Label eqL[8];
        std::unique_ptr<juce::SliderParameterAttachment> eqA[8];
        juce::ComboBox delayTime, reverbTypeBox;
        std::unique_ptr<juce::ComboBoxParameterAttachment> delayTimeA, reverbTypeA;
        juce::Colour bg { 0xff15171c }, accent { 0xffff9a2e }, text_ { 0xffe8eaf0 }, muted { 0xff8b93a7 };
    };

    juce::AudioProcessorEditor* AmpPlugin::createEditor() { return new AmpEditor (*this); }


    // ------------------------------------------------------------ stereo delay (zero latency, see FxCores.h)
    class DelayPlugin : public BuiltinBase
    {
    public:
        DelayPlugin() : BuiltinBase ("delay", "Delay", "Tempo-synced stereo delay with ping-pong, modulation and filters (zero latency)")
        {
            unit = choice ("ut", "Time unit", { "BPM", "HOST", "MS" }, 1);
            ownBpm = addLinear ("bp", "BPM", 20.0f, 300.0f, 120.0f);
            std::vector<juce::String> names;
            for (int i = 0; i < tcfx::kDivisions; ++i)
                names.push_back (tcfx::divisions()[i].name);
            divL = choice ("tl", "Delay L", names, 8);
            divR = choice ("tr", "Delay R", names, 8);
            msL = addFloat ("ml", "Delay L ms", 1.0f, 3000.0f, 400.0f, 0.15f);
            msR = addFloat ("mr", "Delay R ms", 1.0f, 3000.0f, 400.0f, 0.15f);
            mode = choice ("md", "Mode", { "Stereo", "Ping Pong", "Dual" }, 0);
            feedback = addLinear ("fb", "Feedback", 0.0f, 200.0f, 35.0f);
            depth = addLinear ("dp", "Mod depth", 0.0f, 100.0f, 0.0f);
            rate = addFloat ("rt", "Mod rate", 0.1f, 10.0f, 0.6f, 0.2f);
            hipass = addFloat ("hp", "HiPass", 20.0f, 20000.0f, 20.0f, 0.1f);
            lopass = addFloat ("lp", "LoPass", 20.0f, 20000.0f, 20000.0f, 0.1f);
            link = sw ("lk", "Filter link");
            dryWet = addLinear ("dw", "Dry/Wet", 0.0f, 100.0f, 30.0f);
            output = addLinear ("ou", "Output", -18.0f, 18.0f, 0.0f);
            invL = sw ("pl", "Phase L");
            invR = sw ("pr", "Phase R");
        }

        void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buf.getNumSamples(), nch = juce::jmin (2, buf.getNumChannels());
            if (nch == 0 || n == 0)
                return;

            double host = 120.0;
            if (auto* ph = getPlayHead())
                if (auto pos = ph->getPosition())
                    if (auto b = pos->getBpm())
                        host = *b;
            hostBpm.store (host);
            const int u = unit->getIndex();
            const double tempo = u == 0 ? (double) ownBpm->get() : host;

            tcfx::DelayEngine::Params p;
            p.timeL = u == 2 ? msL->get() * 0.001 : tcfx::divisionSeconds (divL->getIndex(), tempo);
            p.timeR = u == 2 ? msR->get() * 0.001 : tcfx::divisionSeconds (divR->getIndex(), tempo);
            p.mode = mode->getIndex();
            p.feedback = feedback->get() * 0.01f;
            p.modDepth = depth->get() * 0.01f;
            p.modRate = rate->get();
            p.hpHz = hipass->get();
            p.lpHz = lopass->get();
            p.invertL = invL->get();
            p.invertR = invR->get();
            engine.setParams (p);
            shownL.store (p.timeL);
            shownR.store (p.mode == tcfx::Dual ? p.timeR : p.timeL);

            const float w = dryWet->get() * 0.01f;
            const float dryT = std::min (1.0f, 2.0f * (1.0f - w)), wetT = std::min (1.0f, 2.0f * w);
            const float outT = std::pow (10.0f, output->get() / 20.0f);
            const float k = 1.0f - std::exp (-1.0f / (0.01f * (float) sr));

            auto* l = buf.getWritePointer (0);
            auto* r = nch > 1 ? buf.getWritePointer (1) : nullptr;
            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                dryG += (dryT - dryG) * k;
                wetG += (wetT - wetG) * k;
                outG += (outT - outG) * k;
                const float inL = l[i], inR = r != nullptr ? r[i] : l[i];
                float wl, wr;
                engine.process (inL, inR, wl, wr);
                if (r == nullptr)
                    l[i] = (inL * dryG + 0.5f * (wl + wr) * wetG) * outG;
                else
                {
                    l[i] = (inL * dryG + wl * wetG) * outG;
                    r[i] = (inR * dryG + wr * wetG) * outG;
                }
                peak = std::max (peak, std::abs (l[i]));
            }
            level.store (std::max (level.load() * 0.9f, peak));
        }

        juce::AudioParameterChoice *unit, *divL, *divR, *mode;
        juce::AudioParameterFloat *ownBpm, *msL, *msR, *feedback, *depth, *rate, *hipass, *lopass, *dryWet, *output;
        juce::AudioParameterBool *link, *invL, *invR;
        std::atomic<double> hostBpm { 120.0 }, shownL { 0.5 }, shownR { 0.5 };
        std::atomic<float> level { 0.0f };
        juce::AudioProcessorEditor* createEditor() override;

    protected:
        void prepare() override
        {
            engine.prepare (sr);
            dryG = std::min (1.0f, 2.0f * (1.0f - dryWet->get() * 0.01f));
            wetG = std::min (1.0f, 2.0f * dryWet->get() * 0.01f);
            outG = std::pow (10.0f, output->get() / 20.0f);
        }

    private:
        juce::AudioParameterChoice* choice (const char* pid, const juce::String& label, std::vector<juce::String> items, int def)
        {
            juce::StringArray a;
            for (auto& s : items)
                a.add (s);
            auto* p = new juce::AudioParameterChoice (juce::ParameterID { pid, 1 }, label, a, def);
            juce::AudioProcessor::addParameter (p);
            return p;
        }
        juce::AudioParameterBool* sw (const char* pid, const juce::String& label)
        {
            auto* p = new juce::AudioParameterBool (juce::ParameterID { pid, 1 }, label, false);
            juce::AudioProcessor::addParameter (p);
            return p;
        }

        tcfx::DelayEngine engine;
        float dryG = 1.0f, wetG = 0.5f, outG = 1.0f;
    };

    class DelayEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit DelayEditor (DelayPlugin& p) : juce::AudioProcessorEditor (p), plug (p)
        {
            setSize (940, 400);
            auto pct = [] (double v) { return juce::String (v, 0) + " %"; };
            auto dB = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; };
            auto divName = [] (double v) { return juce::String (tcfx::divisions()[juce::jlimit (0, tcfx::kDivisions - 1, (int) std::lround (v))].name); };
            auto ms = [] (double v) { return juce::String (v, 0) + " ms"; };
            auto freq = [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 2) + " k" : juce::String (v, 0) + " Hz"; };

            setupKnob (kL, "DELAY L", *plug.divL, divName, 1.0);
            setupKnob (kR, "DELAY R", *plug.divR, divName, 1.0);
            setupKnob (kLms, "DELAY L", *plug.msL, ms, 0.0);
            setupKnob (kRms, "DELAY R", *plug.msR, ms, 0.0);
            setupKnob (kFb, "FEEDBACK", *plug.feedback, pct, 0.0);
            setupKnob (kDepth, "DEPTH", *plug.depth, pct, 0.0);
            setupKnob (kRate, "RATE", *plug.rate, [] (double v) { return juce::String (v, 1) + " Hz"; }, 0.0);
            setupKnob (kHp, "HIPASS", *plug.hipass, freq, 0.0);
            setupKnob (kLp, "LOPASS", *plug.lopass, freq, 0.0);
            setupKnob (kWet, "DRY/WET", *plug.dryWet, pct, 0.0);
            setupKnob (kOut, "OUTPUT", *plug.output, dB, 0.0);
            setupKnob (kBpm, "BPM", *plug.ownBpm, [] (double v) { return juce::String (v, 1); }, 0.0);
            kHp.s.onValueChange = [this] { linkMove (kHp, kLp, true); };
            kLp.s.onValueChange = [this] { linkMove (kLp, kHp, false); };

            for (auto* b : { &tap, &btnStereo, &btnPing, &btnDual, &btnPhL, &btnPhR, &btnBpm, &btnHost, &btnMs, &btnLink })
            {
                b->setClickingTogglesState (false);
                b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff262b36));
                b->setColour (juce::TextButton::buttonOnColourId, accent);
                b->setColour (juce::TextButton::textColourOffId, text_);
                b->setColour (juce::TextButton::textColourOnId, juce::Colour (0xff15171c));
                addAndMakeVisible (b);
            }
            tap.setButtonText ("TAP");
            tap.setTooltip ("Tap the tempo (BPM unit); in MS mode it sets the delay time");
            tap.onClick = [this] { tapTempo(); };
            btnStereo.setButtonText ("STEREO");
            btnPing.setButtonText ("PING PONG");
            btnDual.setButtonText ("DUAL");
            btnPhL.setButtonText (juce::String::fromUTF8 ("\xC3\x98" "L"));
            btnPhR.setButtonText (juce::String::fromUTF8 ("\xC3\x98" "R"));
            btnBpm.setButtonText ("BPM");
            btnHost.setButtonText ("HOST");
            btnMs.setButtonText ("MS");
            btnLink.setButtonText ("LINK");
            btnStereo.onClick = [this] { setChoice (*plug.mode, 0); };
            btnPing.onClick = [this] { setChoice (*plug.mode, 1); };
            btnDual.onClick = [this] { setChoice (*plug.mode, 2); };
            btnPhL.onClick = [this] { toggle (*plug.invL); };
            btnPhR.onClick = [this] { toggle (*plug.invR); };
            btnBpm.onClick = [this] { setChoice (*plug.unit, 0); };
            btnHost.onClick = [this] { setChoice (*plug.unit, 1); };
            btnMs.onClick = [this] { setChoice (*plug.unit, 2); };
            btnLink.onClick = [this] { toggle (*plug.link); };
            btnHost.setTooltip ("The delay follows the tempo of the project");
            btnBpm.setTooltip ("The delay follows its own BPM value");
            btnLink.setTooltip ("Move HiPass and LoPass together");
            startTimerHz (20);
            refresh();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (bg);
            auto body = getLocalBounds().toFloat().reduced (8.0f);
            juce::ColourGradient grad (juce::Colour (0xff23262e), body.getX(), body.getY(), juce::Colour (0xff181a20), body.getX(), body.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (body, 10.0f);
            g.setColour (juce::Colour (0xff3a4152));
            g.drawRoundedRectangle (body, 10.0f, 2.0f);

            g.setColour (accent);
            g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
            g.drawText ("DELAY", 24, 14, 120, 30, juce::Justification::centredLeft);

            // the display in the middle
            auto lcd = lcdRect().toFloat();
            g.setColour (juce::Colour (0xff0b0d11));
            g.fillRoundedRectangle (lcd, 6.0f);
            g.setColour (juce::Colour (0xff3a4152));
            g.drawRoundedRectangle (lcd, 6.0f, 1.0f);
            g.setColour (accent);
            const int u = plug.unit->getIndex();
            const double tempo = u == 0 ? (double) plug.ownBpm->get() : plug.hostBpm.load();
            g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
            g.drawText (u == 2 ? juce::String ("MS") : juce::String (tempo, 1), lcd.withTrimmedBottom (26.0f), juce::Justification::centred);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            if (u != 2)
                g.drawText ("BPM", lcd.withTrimmedTop (50.0f), juce::Justification::centred);
            const auto tl = plug.shownL.load(), tr = plug.shownR.load();
            g.setColour (muted);
            g.drawText ("L  " + juce::String (tl * 1000.0, 0) + " ms", lcd.reduced (10.0f, 6.0f), juce::Justification::topLeft);
            g.drawText ("R  " + juce::String (tr * 1000.0, 0) + " ms", lcd.reduced (10.0f, 6.0f), juce::Justification::topRight);

            // level meter
            auto m = meterRect().toFloat();
            g.setColour (juce::Colour (0xff0b0d11));
            g.fillRoundedRectangle (m, 3.0f);
            const float db = juce::Decibels::gainToDecibels (plug.level.load(), -48.0f);
            const float h = juce::jmap (db, -48.0f, 0.0f, 0.0f, m.getHeight());
            g.setColour (db > -3.0f ? juce::Colour (0xffe0503a) : accent);
            g.fillRoundedRectangle (m.withTop (m.getBottom() - juce::jlimit (0.0f, m.getHeight(), h)), 3.0f);
            g.setColour (muted);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText ("LEVEL", m.getX() - 12, m.getBottom() + 4, m.getWidth() + 24, 12, juce::Justification::centred);

            // group captions
            g.setColour (muted);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("MODULATION", 330, 392 - 150 + 8, 220, 14, juce::Justification::centred);
            g.drawText ("FILTERS", 580, 392 - 150 + 8, 270, 14, juce::Justification::centred);
            g.setColour (juce::Colour (0xff3a4152));
            g.drawLine (340, 262, 540, 262, 1.0f);
            g.drawLine (590, 262, 850, 262, 1.0f);
        }

        void resized() override
        {
            tap.setBounds (24, 64, 84, 84);
            const bool msMode = plug.unit->getIndex() == 2;
            place (kL, { 118, 40, 168, 190 });
            place (kLms, { 118, 40, 168, 190 });
            place (kFb, { 614, 40, 168, 190 });
            place (kWet, { 828, 36, 96, 100 });
            place (kOut, { 828, 146, 96, 100 });
            place (kR, { 143, 252, 118, 130 });
            place (kRms, { 143, 252, 118, 130 });
            place (kBpm, { 24, 164, 84, 100 });
            place (kDepth, { 348, 272, 100, 116 });
            place (kRate, { 448, 272, 100, 116 });
            place (kHp, { 590, 272, 110, 116 });
            place (kLp, { 740, 272, 110, 116 });
            btnLink.setBounds (706, 310, 28, 40);
            juce::ignoreUnused (msMode);

            const auto lcd = lcdRect();
            {   // the two rows of buttons fill the width of the display evenly
                const int widths1[5] = { 34, 66, 80, 50, 34 };
                const int gap = 4;
                int x = lcd.getX();
                juce::TextButton* row1[5] = { &btnPhL, &btnStereo, &btnPing, &btnDual, &btnPhR };
                for (int i = 0; i < 5; ++i)
                {
                    const int w = i == 4 ? lcd.getRight() - x : widths1[i];
                    row1[i]->setBounds (x, lcd.getBottom() + 8, w, 26);
                    x += w + gap;
                }
                const int w3 = (lcd.getWidth() - 2 * 6) / 3;
                btnBpm.setBounds (lcd.getX(), lcd.getBottom() + 44, w3, 26);
                btnHost.setBounds (lcd.getX() + w3 + 6, lcd.getBottom() + 44, w3, 26);
                btnMs.setBounds (lcd.getRight() - w3, lcd.getBottom() + 44, w3, 26);
            }
            updateVisibility();
        }

    private:
        struct Knob
        {
            juce::Slider s;
            juce::Label l;
            std::unique_ptr<juce::SliderParameterAttachment> a;
        };

        juce::Rectangle<int> lcdRect() const { return { 304, 56, 288, 86 }; }
        juce::Rectangle<int> meterRect() const { return { 796, 60, 16, 170 }; }

        void setupKnob (Knob& k, const juce::String& text, juce::RangedAudioParameter& prm, std::function<juce::String (double)> fmt, double interval)
        {
            auto& s = k.s;
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 90, 20);   // read only: the knob is the control
            s.setColour (juce::Slider::rotarySliderFillColourId, accent);
            s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3a4152));
            s.setColour (juce::Slider::thumbColourId, accent);
            s.setColour (juce::Slider::textBoxTextColourId, text_);
            s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            addAndMakeVisible (s);
            k.l.setText (text, juce::dontSendNotification);
            k.l.setJustificationType (juce::Justification::centred);
            k.l.setColour (juce::Label::textColourId, muted);
            k.l.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            addAndMakeVisible (k.l);
            k.a = std::make_unique<juce::SliderParameterAttachment> (prm, s);
            if (interval > 0.0)
                s.setNumDecimalPlacesToDisplay (0);
            s.textFromValueFunction = fmt;
            s.updateText();
        }

        static void place (Knob& k, juce::Rectangle<int> r)
        {
            k.l.setBounds (r.removeFromTop (18));
            k.s.setBounds (r);
        }

        void linkMove (Knob& moved, Knob& other, bool movedIsHp)
        {
            if (linking || ! plug.link->get())
                return;
            // keep the distance of the two cut-offs (in octaves) while the pair slides
            const double hp = kHp.s.getValue(), lp = kLp.s.getValue();
            if (moved.s.isMouseButtonDown())
            {
                linking = true;
                const double ratio = (movedIsHp ? lastLp / lastHp : lastLp / lastHp);
                const double target = movedIsHp ? hp * ratio : lp / ratio;
                other.s.setValue (juce::jlimit (20.0, 20000.0, target), juce::sendNotificationSync);
                linking = false;
            }
            lastHp = kHp.s.getValue();
            lastLp = kLp.s.getValue();
        }

        void tapTempo()
        {
            const auto now = juce::Time::getMillisecondCounterHiRes();
            if (now - lastTap > 2000.0)
                taps.clear();
            lastTap = now;
            taps.push_back (now);
            if (taps.size() > 6)
                taps.erase (taps.begin());
            if (taps.size() < 2)
                return;
            const double interval = (taps.back() - taps.front()) / (double) (taps.size() - 1);   // ms per tap
            if (plug.unit->getIndex() == 2)
            {
                plug.msL->beginChangeGesture();
                *plug.msL = (float) juce::jlimit (1.0, 3000.0, interval);
                plug.msL->endChangeGesture();
                plug.msR->beginChangeGesture();
                *plug.msR = (float) juce::jlimit (1.0, 3000.0, interval);
                plug.msR->endChangeGesture();
            }
            else
            {
                setChoice (*plug.unit, 0);
                plug.ownBpm->beginChangeGesture();
                *plug.ownBpm = (float) juce::jlimit (20.0, 300.0, 60000.0 / interval);
                plug.ownBpm->endChangeGesture();
            }
        }

        static void setChoice (juce::AudioParameterChoice& p, int i)
        {
            p.beginChangeGesture();
            p = i;
            p.endChangeGesture();
        }
        static void toggle (juce::AudioParameterBool& p)
        {
            p.beginChangeGesture();
            p = ! p.get();
            p.endChangeGesture();
        }

        void updateVisibility()
        {
            const bool ms = plug.unit->getIndex() == 2;
            const bool dual = plug.mode->getIndex() == 2;
            kL.s.setVisible (! ms); kL.l.setVisible (! ms);
            kLms.s.setVisible (ms); kLms.l.setVisible (ms);
            kR.s.setVisible (! ms); kR.l.setVisible (! ms);
            kRms.s.setVisible (ms); kRms.l.setVisible (ms);
            for (auto* c : { (juce::Component*) &kR.s, (juce::Component*) &kRms.s })
                c->setEnabled (dual);
            kBpm.s.setVisible (plug.unit->getIndex() == 0);
            kBpm.l.setVisible (plug.unit->getIndex() == 0);
            kL.l.setText (dual || plug.mode->getIndex() == 1 ? "DELAY" : "DELAY", juce::dontSendNotification);
        }

        void refresh()
        {
            btnStereo.setToggleState (plug.mode->getIndex() == 0, juce::dontSendNotification);
            btnPing.setToggleState (plug.mode->getIndex() == 1, juce::dontSendNotification);
            btnDual.setToggleState (plug.mode->getIndex() == 2, juce::dontSendNotification);
            btnPhL.setToggleState (plug.invL->get(), juce::dontSendNotification);
            btnPhR.setToggleState (plug.invR->get(), juce::dontSendNotification);
            btnBpm.setToggleState (plug.unit->getIndex() == 0, juce::dontSendNotification);
            btnHost.setToggleState (plug.unit->getIndex() == 1, juce::dontSendNotification);
            btnMs.setToggleState (plug.unit->getIndex() == 2, juce::dontSendNotification);
            btnLink.setToggleState (plug.link->get(), juce::dontSendNotification);
            if (plug.unit->getIndex() != lastUnit || plug.mode->getIndex() != lastMode)
            {
                lastUnit = plug.unit->getIndex();
                lastMode = plug.mode->getIndex();
                updateVisibility();
            }
            if (! kHp.s.isMouseButtonDown() && ! kLp.s.isMouseButtonDown())
            {
                lastHp = kHp.s.getValue();
                lastLp = kLp.s.getValue();
            }
        }

        void timerCallback() override
        {
            refresh();
            repaint();
        }

        DelayPlugin& plug;
        Knob kL, kR, kLms, kRms, kFb, kDepth, kRate, kHp, kLp, kWet, kOut, kBpm;
        juce::TextButton tap, btnStereo, btnPing, btnDual, btnPhL, btnPhR, btnBpm, btnHost, btnMs, btnLink;
        std::vector<double> taps;
        double lastTap = 0.0, lastHp = 20.0, lastLp = 20000.0;
        int lastUnit = -1, lastMode = -1;
        bool linking = false;
        juce::Colour bg { 0xff15171c }, accent { 0xffff9a2e }, text_ { 0xffe8eaf0 }, muted { 0xff8b93a7 };
    };

    juce::AudioProcessorEditor* DelayPlugin::createEditor() { return new DelayEditor (*this); }

    // ------------------------------------------------------------ autotune (short bounded delay, see AutotuneCore.h)
    namespace tunekeys
    {
        const char* const noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const char* const scaleNames[5] = { "Major", "Minor", "Major Pentatonic", "Minor Pentatonic", "Chromatic" };
        const int scaleSteps[5][12] = { { 0, 2, 4, 5, 7, 9, 11, -1, -1, -1, -1, -1 }, { 0, 2, 3, 5, 7, 8, 10, -1, -1, -1, -1, -1 },
                                        { 0, 2, 4, 7, 9, -1, -1, -1, -1, -1, -1, -1 }, { 0, 3, 5, 7, 10, -1, -1, -1, -1, -1, -1, -1 },
                                        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } };

        // key index: 0 = Chromatic, 1 + root * 4 + scale (scale 0..3)
        inline std::uint16_t maskFor (int key)
        {
            if (key <= 0)
                return 0x0FFF;
            const int root = (key - 1) / 4, sc = (key - 1) % 4;
            std::uint16_t m = 0;
            for (int s : scaleSteps[sc])
                if (s >= 0)
                    m |= (std::uint16_t) (1u << ((root + s) % 12));
            return m;
        }
        inline juce::String keyName (int key)
        {
            if (key <= 0)
                return "Chromatic";
            return juce::String (noteNames[(key - 1) / 4]) + " " + scaleNames[(key - 1) % 4];
        }
    }

    class TunePlugin : public BuiltinBase
    {
    public:
        TunePlugin() : BuiltinBase ("tune", "Autotune", "Pitch correction with note keyboard, retune speed and choir voices")
        {
            retune = addFloat ("rt", "Retune Speed", 0.0f, 400.0f, 30.0f, 0.12f);
            human = addLinear ("hu", "Humanize", 0.0f, 100.0f, 10.0f);
            flex = addLinear ("fx", "Flex Tune", 0.0f, 100.0f, 15.0f);
            harmLevel = addLinear ("hl", "Harmony", 0.0f, 100.0f, 0.0f);   // the choir is off until the player turns it up
            widthP = addLinear ("wd", "Width", 0.0f, 100.0f, 60.0f);
            synthP = addLinear ("sy", "Synth", 0.0f, 100.0f, 0.0f);
            detune = addLinear ("de", "Detune", 430.0f, 450.0f, 440.0f);
            classic = new juce::AudioParameterBool (juce::ParameterID { "cl", 1 }, "Classic", false);
            hq = new juce::AudioParameterBool (juce::ParameterID { "hq", 1 }, "HQ", false);
            on = new juce::AudioParameterBool (juce::ParameterID { "on", 1 }, "On", true);
            followProject = new juce::AudioParameterBool (juce::ParameterID { "pk", 1 }, "Project key", false);
            range = new juce::AudioParameterChoice (juce::ParameterID { "ra", 1 }, "Vocal Range",
                                                    juce::StringArray { "Soprano", "Alto/Tenor", "Low Male", "Instrument" }, 1);
            key = new juce::AudioParameterInt (juce::ParameterID { "ky", 1 }, "Key", 0, 48, 0);
            for (auto* p : { (juce::AudioProcessorParameter*) classic, (juce::AudioProcessorParameter*) hq, (juce::AudioProcessorParameter*) on,
                             (juce::AudioProcessorParameter*) range, (juce::AudioProcessorParameter*) key,
                             (juce::AudioProcessorParameter*) followProject })
                juce::AudioProcessor::addParameter (p);
            for (int k = 0; k < 4; ++k)
            {
                harm[k] = new juce::AudioParameterChoice (juce::ParameterID { "h" + juce::String (k), 1 }, "Voice " + juce::String (k + 1),
                                                          juce::StringArray { "Off", "-Oct", "-5th", "-3rd", "+3rd", "+5th", "+Oct" }, 0);
                juce::AudioProcessor::addParameter (harm[k]);
            }
            for (int i = 0; i < 12; ++i)
            {
                note[i] = new juce::AudioParameterBool (juce::ParameterID { "n" + juce::String (i), 1 }, juce::String (tunekeys::noteNames[i]), true);
                juce::AudioProcessor::addParameter (note[i]);
            }
        }

        void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buf.getNumSamples(), nch = juce::jmin (2, buf.getNumChannels());
            if (nch == 0)
                return;

            tctune::Params p;
            p.retuneMs = retune->get();
            p.humanize = human->get();
            p.flex = flex->get();
            p.classic = classic->get();
            p.hq = hq->get();
            p.refHz = detune->get();
            static const int steps[7] = { 0, -7, -4, -2, 2, 4, 7 };   // scale steps: octave 7, fifth 4, third 2
            for (int k = 0; k < 4; ++k)
                p.harmony[k] = steps[harm[k]->getIndex()];
            p.harmLevel = harmLevel->get();
            p.width = widthP->get();
            p.synth = synthP->get();
            p.mask = effectiveMask();
            static const float lows[4] = { 140.0f, 80.0f, 60.0f, 45.0f }, highs[4] = { 1100.0f, 700.0f, 450.0f, 1500.0f };
            p.rangeLowHz = lows[range->getIndex()];
            p.rangeHighHz = highs[range->getIndex()];
            core.setParams (p);
            if (p.hq != lastHq)
            {
                lastHq = p.hq;
                setLatencySamples (core.getTypicalLatency (p.hq));
            }

            auto* l = buf.getWritePointer (0);
            auto* r = nch > 1 ? buf.getWritePointer (1) : nullptr;
            if (dry.size() < (size_t) n * 2)
                dry.resize ((size_t) n * 2);
            std::copy (l, l + n, dry.begin());
            if (r != nullptr)
                std::copy (r, r + n, dry.begin() + n);

            core.process (l, r, n);

            // bypass: short cross-fade to the untouched input, the correction keeps running
            const float target = on->get() ? 1.0f : 0.0f;
            const float step = (float) (1.0 / (0.012 * sr));
            float w = ramp;
            for (int i = 0; i < n; ++i)
            {
                w += juce::jlimit (-step, step, target - w);
                l[i] = l[i] * w + dry[(size_t) i] * (1.0f - w);
                if (r != nullptr)
                    r[i] = r[i] * w + dry[(size_t) (n + i)] * (1.0f - w);
            }
            ramp = w;
        }

        // the notes the correction may use: the keyboard, or the key of the project when that box is ticked and a key is known
        bool followingProject() const { return followProject->get() && tcshared::project().keyTonic.load() >= 0; }
        std::uint16_t effectiveMask() const
        {
            if (followingProject())
                return tunekeys::maskFor (1 + tcshared::project().keyTonic.load() * 4 + (tcshared::project().keyMinor.load() ? 1 : 0));
            std::uint16_t m = 0;
            for (int i = 0; i < 12; ++i)
                if (note[i]->get())
                    m |= (std::uint16_t) (1u << i);
            return m;
        }

        juce::AudioParameterFloat *retune, *human, *flex, *harmLevel, *widthP, *synthP, *detune;
        juce::AudioParameterChoice* harm[4];
        juce::AudioParameterBool *classic, *hq, *on, *followProject, *note[12];
        juce::AudioParameterChoice* range;
        juce::AudioParameterInt* key;
        tctune::Core core;
        juce::AudioProcessorEditor* createEditor() override;

    protected:
        void prepare() override
        {
            core.prepare (sr);
            lastHq = hq->get();
            setLatencySamples (core.getTypicalLatency (lastHq));
            ramp = on->get() ? 1.0f : 0.0f;
        }

    private:
        std::vector<float> dry;
        bool lastHq = false;
        float ramp = 1.0f;
    };

    class TuneLook : public juce::LookAndFeel_V4
    {
    public:
        void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s) override
        {
            auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (6.0f);
            const float d = juce::jmin (r.getWidth(), r.getHeight());
            r = juce::Rectangle<float> (d, d).withCentre (r.getCentre());
            const auto accent = s.findColour (juce::Slider::rotarySliderFillColourId);
            const float ang = a0 + pos * (a1 - a0);

            juce::Path track, val;
            track.addCentredArc (r.getCentreX(), r.getCentreY(), d * 0.5f, d * 0.5f, 0.0f, a0, a1, true);
            val.addCentredArc (r.getCentreX(), r.getCentreY(), d * 0.5f, d * 0.5f, 0.0f, a0, ang, true);
            g.setColour (juce::Colour (0xff1d2428));
            g.strokePath (track, juce::PathStrokeType (3.0f));
            g.setColour (accent);
            g.strokePath (val, juce::PathStrokeType (3.0f));

            auto body = r.reduced (d * 0.1f);
            juce::ColourGradient grad (juce::Colour (0xff3b4248), body.getX(), body.getY(), juce::Colour (0xff22282c), body.getX(), body.getBottom(), false);
            g.setGradientFill (grad);
            g.fillEllipse (body);
            g.setColour (juce::Colour (0xff12161a));
            g.drawEllipse (body, 1.5f);
            const auto c = body.getCentre();
            const float rad = body.getWidth() * 0.5f;
            g.setColour (juce::Colour (0xffd9dde0));
            g.drawLine (c.x + std::sin (ang) * rad * 0.45f, c.y - std::cos (ang) * rad * 0.45f,
                        c.x + std::sin (ang) * rad * 0.82f, c.y - std::cos (ang) * rad * 0.82f, 2.5f);
        }
    };

    class TuneEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit TuneEditor (TunePlugin& p) : juce::AudioProcessorEditor (p), plug (p)
        {
            setLookAndFeel (&look);
            setSize (860, 600);

            knob (rtS, "Retune Speed", *p.retune, rtA, teal, 0);
            knob (huS, "Humanize", *p.human, huA, juce::Colour (0xff8a63d2), 0);
            knob (fxS, "Flex Tune", *p.flex, fxA, juce::Colour (0xffe6d94a), 0);
            knob (voS, "Level", *p.harmLevel, voA, juce::Colour (0xff4fb3ff), 0);
            knob (spS, "Width", *p.widthP, spA, juce::Colour (0xffff9a4d), 0);
            knob (syS, "Synth", *p.synthP, syA, juce::Colour (0xffe05cc0), 0);
            for (int k = 0; k < 4; ++k)
            {
                harmBtn[k].setColour (juce::TextButton::textColourOffId, juce::Colours::white);
                harmBtn[k].onClick = [this, k]
                {
                    juce::PopupMenu m;
                    for (int i = 0; i < plug.harm[k]->choices.size(); ++i)
                        m.addItem (i + 1, plug.harm[k]->choices[i], true, plug.harm[k]->getIndex() == i);
                    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&harmBtn[k]), [this, k] (int r)
                    {
                        if (r > 0)
                        {
                            plug.harm[k]->beginChangeGesture();
                            *plug.harm[k] = r - 1;
                            plug.harm[k]->endChangeGesture();
                            refreshTexts();
                        }
                    });
                };
                harmBtn[k].setTooltip ("Harmony voice: interval above / below the sung note, in steps of the chosen scale");
                addAndMakeVisible (harmBtn[k]);
            }

            followBtn.setButtonText ("Project key");
            followBtn.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
            followBtn.setColour (juce::ToggleButton::tickColourId, teal);
            followBtn.setColour (juce::ToggleButton::tickDisabledColourId, muted);
            followBtn.setTooltip ("Follow the key of the project (the Key box of the window). While no key is known, the keyboard below is used");
            addAndMakeVisible (followBtn);
            followA = std::make_unique<juce::ButtonParameterAttachment> (*p.followProject, followBtn);

            detuneS.setSliderStyle (juce::Slider::LinearBar);
            detuneS.setTextBoxIsEditable (true);
            detuneS.setColour (juce::Slider::trackColourId, juce::Colours::transparentBlack);
            detuneS.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
            detuneS.setTextValueSuffix (" Hz");
            detuneS.setNumDecimalPlacesToDisplay (1);
            detuneS.setTooltip ("Reference pitch (A4). Drag, or double-click to type; double-click resets to 440");
            addAndMakeVisible (detuneS);
            detuneA = std::make_unique<juce::SliderParameterAttachment> (*p.detune, detuneS);
            detuneS.setDoubleClickReturnValue (true, 440.0);

            for (auto* b : { &rangeBtn, &keyBtn })
            {
                b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1a2125));
                b->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
                addAndMakeVisible (b);
            }
            rangeBtn.onClick = [this]
            {
                juce::PopupMenu m;
                for (int i = 0; i < plug.range->choices.size(); ++i)
                    m.addItem (i + 1, plug.range->choices[i], true, plug.range->getIndex() == i);
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&rangeBtn), [this] (int r)
                {
                    if (r > 0)
                    {
                        plug.range->beginChangeGesture();
                        *plug.range = r - 1;
                        plug.range->endChangeGesture();
                        refreshTexts();
                    }
                });
            };
            keyBtn.onClick = [this]
            {
                juce::PopupMenu m;
                m.addItem (1, "Chromatic", true, plug.key->get() == 0);
                for (int root = 0; root < 12; ++root)
                {
                    juce::PopupMenu sub;
                    for (int sc = 0; sc < 4; ++sc)
                    {
                        const int k = 1 + root * 4 + sc;
                        sub.addItem (k + 1, tunekeys::scaleNames[sc], true, plug.key->get() == k);
                    }
                    m.addSubMenu (tunekeys::noteNames[root], sub);
                }
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&keyBtn), [this] (int r)
                {
                    if (r > 0)
                        setKey (r - 1);
                });
            };

            for (auto* b : { &modeLL, &modeHQ, &styleModern, &styleClassic })
            {
                b->setClickingTogglesState (false);
                addAndMakeVisible (b);
            }
            modeLL.onClick = [this] { setBool (*plug.hq, false); };
            modeHQ.onClick = [this] { setBool (*plug.hq, true); };
            styleModern.onClick = [this] { setBool (*plug.classic, false); };
            styleClassic.onClick = [this] { setBool (*plug.classic, true); };
            modeLL.setTooltip ("Shorter crossfades");
            modeHQ.setTooltip ("Longer crossfades, smoother sound");
            styleModern.setTooltip ("Humanize and Flex Tune work");
            styleClassic.setTooltip ("Plain retune speed, Humanize and Flex Tune are ignored");

            refreshTexts();
            startTimerHz (30);
        }

        ~TuneEditor() override { setLookAndFeel (nullptr); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff0e1214));
            auto left = leftPanel().toFloat(), right = rightPanel().toFloat();
            for (auto* r : { &left, &right })
            {
                juce::ColourGradient grad (juce::Colour (0xff1b2226), r->getX(), r->getY(), juce::Colour (0xff12171a), r->getX(), r->getBottom(), false);
                g.setGradientFill (grad);
                g.fillRoundedRectangle (*r, 12.0f);
                g.setColour (juce::Colour (0xff0a0d0f));
                g.drawRoundedRectangle (*r, 12.0f, 1.5f);
            }

            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
            g.drawText ("Autotune", 40, 26, 160, 34, juce::Justification::centredLeft);
            g.setColour (muted);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("Vocal Range", rangeBtn.getX() + 8, rangeBtn.getY() - 12, 100, 12, juce::Justification::centredLeft);
            g.drawText ("Key & Scale", keyBtn.getX() + 8, keyBtn.getY() - 12, 100, 12, juce::Justification::centredLeft);

            // big note
            const int target = plug.core.outTarget.load();
            const bool voiced = plug.core.outVoiced.load();
            juce::String letter = target >= 0 && voiced ? juce::String (tunekeys::noteNames[target]).substring (0, 1) : "-";
            const bool sharp = target >= 0 && voiced && juce::String (tunekeys::noteNames[target]).contains ("#");
            g.setColour (voiced ? juce::Colours::white : juce::Colour (0xff4a555c));
            g.setFont (juce::FontOptions (104.0f, juce::Font::plain));
            g.drawText (letter, 219, 74, 180, 120, juce::Justification::centred);
            if (sharp)
            {
                g.setFont (juce::FontOptions (40.0f));
                g.drawText (juce::String::fromUTF8 ("\xE2\x99\xAF"), 359, 78, 50, 50, juce::Justification::centred);
            }

            // cents bar
            auto bar = meterRect().toFloat();
            g.setColour (juce::Colour (0xff0b0f11));
            g.fillRoundedRectangle (bar, 4.0f);
            const float cents = juce::jlimit (-100.0f, 100.0f, voiced ? plug.core.outInCents.load() : 0.0f);
            const float cx = bar.getCentreX();
            const float x = cx + cents / 100.0f * bar.getWidth() * 0.5f;
            if (voiced)
            {
                g.setColour (teal);
                g.fillRect (juce::Rectangle<float> (juce::jmin (cx, x) - 4.0f, bar.getY() + 2.0f, std::abs (x - cx) + 8.0f, bar.getHeight() - 4.0f));
            }
            g.setColour (juce::Colour (0xffd4e06b));
            g.fillRect (bar.getX() + 3.0f, bar.getY() - 2.0f, 2.5f, bar.getHeight() + 4.0f);
            g.fillRect (bar.getRight() - 5.5f, bar.getY() - 2.0f, 2.5f, bar.getHeight() + 4.0f);
            g.setColour (muted);
            g.setFont (juce::FontOptions (11.0f));
            for (int c = -100; c <= 100; c += 10)
            {
                const float tx = cx + (float) c / 100.0f * bar.getWidth() * 0.5f;
                g.drawLine (tx, bar.getBottom() + 6.0f, tx, bar.getBottom() + (c % 50 == 0 ? 14.0f : 10.0f), 1.0f);
            }
            for (int c = -100; c <= 100; c += 50)
                g.drawText ((c > 0 ? "+" : "") + juce::String (c), (int) (cx + (float) c / 100.0f * bar.getWidth() * 0.5f) - 20, (int) bar.getBottom() + 16, 40, 14,
                            juce::Justification::centred);

            // keys
            g.setColour (muted);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Edit Notes", 40, 286, 100, 18, juce::Justification::centredLeft);
            g.drawText (juce::String::fromUTF8 ("\xE2\x86\xBA"), resetRect().toNearestInt(), juce::Justification::centred);
            const std::uint16_t effMask = plug.effectiveMask();
            for (int i = 0; i < 12; ++i)
            {
                auto k = keyRect (i).toFloat();
                const bool allowed = (effMask & (1u << i)) != 0;
                const bool hot = voiced && target == i;
                g.setColour (hot ? teal.withAlpha (0.9f) : allowed ? teal.withAlpha (0.18f) : juce::Colour (0xff161c1f));
                g.fillRoundedRectangle (k, k.getWidth() * 0.4f);
                g.setColour (allowed ? teal : juce::Colour (0xff2a3338));
                g.drawRoundedRectangle (k.reduced (0.5f), k.getWidth() * 0.4f, 1.5f);
                g.setColour (hot ? juce::Colours::black : allowed ? juce::Colours::white : juce::Colour (0xff5b676d));
                g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
                g.drawText (tunekeys::noteNames[i], k.toNearestInt(), juce::Justification::centred);
            }

            // bottom of the left panel (below the keys, nothing overlaps them)
            g.setColour (muted);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("Detune", detuneS.getX(), detuneS.getY() - 16, 80, 12, juce::Justification::centredLeft);
            g.drawText ("Mode", modeLL.getX(), modeLL.getY() - 16, 80, 12, juce::Justification::centredLeft);
            g.setColour (juce::Colour (0xff2a3338));
            g.drawLine (30.0f, 460.0f, left.getRight() - 20.0f, 460.0f);

            // right panel
            for (auto* s : { &rtS, &huS, &fxS, &voS, &spS, &syS })
            {
                g.setColour (muted.brighter (0.4f));
                g.setFont (juce::FontOptions (s == &rtS ? 16.0f : 13.0f));
                g.drawText (s->getName(), s->getX() - 20, s->getY() - (s == &rtS ? 22 : 18), s->getWidth() + 40, 18, juce::Justification::centred);
            }

            // the harmony block: the three knobs and the four voices belong together
            const auto hb = harmonyBlock().toFloat();
            const auto blue = juce::Colour (0xff4fb3ff);
            g.setColour (blue.withAlpha (0.06f));
            g.fillRoundedRectangle (hb, 10.0f);
            g.setColour (blue.withAlpha (0.45f));
            g.drawRoundedRectangle (hb.reduced (0.5f), 10.0f, 1.2f);
            g.setColour (blue);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText ("HARMONY", (int) hb.getX() + 12, (int) hb.getY() + 5, 120, 14, juce::Justification::centredLeft);
        }

        void resized() override
        {
            rangeBtn.setBounds (260, 36, 130, 28);
            keyBtn.setBounds (404, 36, 150, 28);
            followBtn.setBounds (404, 68, 150, 24);
            detuneS.setBounds (122, 484, 110, 28);
            modeLL.setBounds (322, 484, 110, 26);
            modeHQ.setBounds (436, 484, 60, 26);

            auto r = rightPanel();
            rtS.setBounds (r.getX() + 30, 70, 170, 170);
            huS.setBounds (r.getX() + 22, 296, 84, 84);
            fxS.setBounds (r.getX() + 124, 296, 84, 84);
            const auto hb = harmonyBlock();
            voS.setBounds (hb.getX() + 6, hb.getY() + 40, 64, 78);
            spS.setBounds (hb.getX() + 75, hb.getY() + 40, 64, 78);
            syS.setBounds (hb.getX() + 144, hb.getY() + 40, 64, 78);
            for (int k = 0; k < 4; ++k)
                harmBtn[k].setBounds (hb.getX() + 8 + (k % 2) * 101, hb.getY() + 126 + (k / 2) * 30, 97, 26);
            styleModern.setBounds (r.getX() + 13, 250, 98, 26);
            styleClassic.setBounds (r.getX() + 119, 250, 98, 26);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            for (int i = 0; i < 12; ++i)
                if (keyRect (i).contains (e.position))
                {
                    if (plug.followingProject())
                        return;   // the project key decides
                    plug.note[i]->beginChangeGesture();
                    *plug.note[i] = ! plug.note[i]->get();
                    plug.note[i]->endChangeGesture();
                    return;
                }
            if (resetRect().contains (e.position) && ! plug.followingProject())
                setKey (plug.key->get());
        }

    private:
        juce::Rectangle<int> leftPanel() const { return { 14, 14, 590, getHeight() - 28 }; }
        juce::Rectangle<int> rightPanel() const { return { 616, 14, 230, getHeight() - 28 }; }
        juce::Rectangle<int> meterRect() const { return { 49, 214, 520, 30 }; }
        juce::Rectangle<float> keyRect (int i) const
        {
            // white keys C D E F G A B on the lower row, black keys between them on the upper row
            static const int white[7] = { 0, 2, 4, 5, 7, 9, 11 };
            const float w = 62.0f, h = 62.0f, gap = 8.0f, x0 = 68.0f;
            for (int k = 0; k < 7; ++k)
                if (white[k] == i)
                    return { x0 + (float) k * (w + gap), 372.0f - 0.0f, w, h - 0.0f };
            static const int black[5] = { 1, 3, 6, 8, 10 };
            static const float pos[5] = { 0.5f, 1.5f, 3.5f, 4.5f, 5.5f };
            for (int k = 0; k < 5; ++k)
                if (black[k] == i)
                    return { x0 + pos[k] * (w + gap) + 2.0f, 304.0f, 54.0f, 54.0f };
            return {};
        }
        juce::Rectangle<float> resetRect() const { return { 545.0f, 284.0f, 24.0f, 22.0f }; }
        juce::Rectangle<int> harmonyBlock() const { return { rightPanel().getX() + 8, 392, rightPanel().getWidth() - 16, getHeight() - 28 - 392 + 4 }; }

        void knob (juce::Slider& s, const juce::String& name, juce::RangedAudioParameter& prm, std::unique_ptr<juce::SliderParameterAttachment>& att, juce::Colour c, int)
        {
            s.setName (name);
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
            s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 20);
            s.setColour (juce::Slider::rotarySliderFillColourId, c);
            s.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
            s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff0b0f11));
            s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff2a3338));
            s.setNumDecimalPlacesToDisplay (0);
            addAndMakeVisible (s);
            att = std::make_unique<juce::SliderParameterAttachment> (prm, s);
        }

        static void setBool (juce::AudioParameterBool& p, bool v)
        {
            p.beginChangeGesture();
            p = v;
            p.endChangeGesture();
        }

        void setKey (int k)
        {
            plug.key->beginChangeGesture();
            *plug.key = k;
            plug.key->endChangeGesture();
            const auto m = tunekeys::maskFor (k);
            for (int i = 0; i < 12; ++i)
                setBool (*plug.note[i], (m & (1u << i)) != 0);
            refreshTexts();
        }

        void refreshTexts()
        {
            rangeBtn.setButtonText (plug.range->getCurrentChoiceName());
            juce::String name = tunekeys::keyName (plug.key->get());
            std::uint16_t cur = 0;
            for (int i = 0; i < 12; ++i)
                if (plug.note[i]->get())
                    cur |= (std::uint16_t) (1u << i);
            if (cur != tunekeys::maskFor (plug.key->get()))
                name = "Custom";
            const bool following = plug.followingProject();
            if (following)
                name = "Project: " + juce::String (tunekeys::noteNames[tcshared::project().keyTonic.load()]) + (tcshared::project().keyMinor.load() ? "m" : "");
            else if (plug.followProject->get())
                name += "  (no project key)";
            keyBtn.setButtonText (name);
            keyBtn.setEnabled (! following);
            for (int k = 0; k < 4; ++k)
            {
                const int idx = plug.harm[k]->getIndex();
                harmBtn[k].setButtonText (idx == 0 ? "Voice " + juce::String (k + 1) + ": off" : "Voice " + juce::String (k + 1) + ": " + plug.harm[k]->choices[idx]);
                harmBtn[k].setColour (juce::TextButton::buttonColourId, idx == 0 ? juce::Colour (0xff161c1f) : juce::Colour (0xff4fb3ff).withAlpha (0.3f));
            }

            auto style = [] (juce::TextButton& b, bool on, juce::Colour c)
            {
                b.setColour (juce::TextButton::buttonColourId, on ? c.withAlpha (0.28f) : juce::Colour (0xff161c1f));
                b.setColour (juce::TextButton::textColourOffId, on ? juce::Colours::white : juce::Colour (0xff7c8990));
            };
            modeLL.setButtonText ("Low Latency");
            modeHQ.setButtonText ("HQ");
            styleModern.setButtonText ("Modern");
            styleClassic.setButtonText ("Classic");
            style (modeLL, ! plug.hq->get(), teal);
            style (modeHQ, plug.hq->get(), teal);
            style (styleModern, ! plug.classic->get(), teal);
            style (styleClassic, plug.classic->get(), teal);
        }

        void timerCallback() override
        {
            refreshTexts();
            repaint();
        }

        TunePlugin& plug;
        TuneLook look;
        juce::Slider rtS, huS, fxS, voS, spS, syS, detuneS;
        juce::ToggleButton followBtn;
        std::unique_ptr<juce::ButtonParameterAttachment> followA;
        std::unique_ptr<juce::SliderParameterAttachment> rtA, huA, fxA, voA, spA, syA, detuneA;
        juce::TextButton harmBtn[4];
        juce::TextButton rangeBtn, keyBtn, modeLL, modeHQ, styleModern, styleClassic;
        const juce::Colour teal { 0xff19d3b0 }, muted { 0xff8b97a0 };
    };

    juce::AudioProcessorEditor* TunePlugin::createEditor() { return new TuneEditor (*this); }

    std::unique_ptr<juce::AudioPluginInstance> makeBuiltin (const juce::String& id)
    {
        if (id == "TC:eq")
            return std::make_unique<EqPlugin>();
        if (id == "TC:comp")
            return std::make_unique<CompPlugin>();
        if (id == "TC:gate")
            return std::make_unique<GatePlugin>();
        if (id == "TC:reverb")
            return std::make_unique<ReverbPlugin>();
        if (id == "TC:od808")
            return std::make_unique<OverdrivePlugin>();
        if (id == "TC:amp")
            return std::make_unique<AmpPlugin>();
        if (id == "TC:tune")
            return std::make_unique<TunePlugin>();
        if (id == "TC:delay")
            return std::make_unique<DelayPlugin>();
        return nullptr;
    }

    const char* const allIds[] = { "TC:eq", "TC:comp", "TC:gate", "TC:reverb", "TC:od808", "TC:amp", "TC:tune", "TC:delay" };
}

std::vector<tcshared::Guitarist> tcshared::guitarists()
{
    std::vector<Guitarist> out;
    const juce::ScopedLock sl (ampRegistryLock());
    bool seen[4] = {};
    for (auto* a : ampRegistry())
    {
        const int slot = juce::jlimit (0, 3, a->guitaristSlot());
        if (seen[slot])
            continue;
        seen[slot] = true;
        Guitarist g;
        g.slot = slot;
        g.name = a->guitaristName();
        g.scene = a->activeScene();
        for (int i = 0; i < 4; ++i)
            g.sceneNames.add (a->getSceneName (i));
        out.push_back (g);
    }
    std::sort (out.begin(), out.end(), [] (const Guitarist& x, const Guitarist& y) { return x.slot < y.slot; });
    return out;
}

bool tcshared::setGuitarScene (int slot, int scene)
{
    bool found = false;
    const juce::ScopedLock sl (ampRegistryLock());
    for (auto* a : ampRegistry())
        if (a->guitaristSlot() == slot)
        {
            a->setScene (scene);
            found = true;
        }
    return found;
}

std::vector<juce::PluginDescription> BuiltinFormat::allDescriptions()
{
    std::vector<juce::PluginDescription> list;
    for (auto* id : allIds)
        if (auto p = makeBuiltin (id))
        {
            juce::PluginDescription d;
            p->fillInPluginDescription (d);
            list.push_back (d);
        }
    return list;
}

bool BuiltinFormat::fileMightContainThisPluginType (const juce::String& fileOrIdentifier)
{
    return fileOrIdentifier.startsWith ("TC:");
}

void BuiltinFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& fileOrIdentifier)
{
    if (auto p = makeBuiltin (fileOrIdentifier))
    {
        auto* d = new juce::PluginDescription();
        p->fillInPluginDescription (*d);
        results.add (d);
    }
}

void BuiltinFormat::createPluginInstance (const juce::PluginDescription& d, double, int, PluginCreationCallback callback)
{
    if (auto p = makeBuiltin (d.fileOrIdentifier))
        callback (std::move (p), {});
    else
        callback (nullptr, "Unknown built-in effect");
}
