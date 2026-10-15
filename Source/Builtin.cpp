#include "Builtin.h"
#include "ReverbCore.h"
#include "OverdriveCore.h"
#include "AmpCore.h"
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

            thA = std::make_unique<juce::SliderParameterAttachment> (*p.thresh, thS);
            raA = std::make_unique<juce::SliderParameterAttachment> (*p.ratio, raS);
            atA = std::make_unique<juce::SliderParameterAttachment> (*p.attack, atS);
            reA = std::make_unique<juce::SliderParameterAttachment> (*p.release, reS);
            rnA = std::make_unique<juce::SliderParameterAttachment> (*p.relNote, rnS);
            ouA = std::make_unique<juce::SliderParameterAttachment> (*p.output, ouS);
            mxA = std::make_unique<juce::SliderParameterAttachment> (*p.mix, mxS);
            bpA = std::make_unique<juce::SliderParameterAttachment> (*p.bpm, bpS);

            thS.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " dB"; };
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
                // GR: 0 dB on the right, 20 dB on the left; IN/OUT: -40 dB on the left, +3 dB on the right
                const float t = (float) i / (float) ticks;
                const bool major = mode == 1 ? (i % 5 == 0) : (i % 10 == 3 || i == ticks);
                const bool red = mode == 1 ? false : (i >= 40);
                g.setColour (red ? juce::Colour (0xffc0392b) : juce::Colour (0xff2b2417));
                g.drawLine (juce::Line<float> (pt (t, rad - (major ? 16.0f : 9.0f)), pt (t, rad - 2.0f)), major ? 2.0f : 1.0f);
                if (major)
                {
                    const int val = mode == 1 ? 20 - i : -40 + i;
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
            const float target = m == 1 ? juce::jlimit (0.0f, 1.0f, 1.0f + grShown / 20.0f)
                                        : juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (m == 0 ? inShown : outShown, -60.0f) + 40.0f) / 43.0f);
            needle += (target - needle) * (m == 1 ? 0.7f : 0.35f);
            repaint (meterArea.expanded (6, 24));
        }

        CompPlugin& plug;
        juce::Slider thS, raS, atS, reS, rnS, ouS, mxS, bpS;
        juce::Label thL, raL, atL, reL, rnL, ouL, mxL, bpL;
        std::unique_ptr<juce::SliderParameterAttachment> thA, raA, atA, reA, rnA, ouA, mxA, bpA;
        juce::TextButton syncMs, syncBpm, syncHost, mIn, mGr, mOut, limBtn;
        std::unique_ptr<juce::ButtonParameterAttachment> limA;
        juce::Rectangle<int> meterArea;
        float inShown = 0.0f, outShown = 0.0f, grShown = 0.0f, needle = 1.0f;
        juce::Colour bg { 0xff15171c }, bg2 { 0xff262b36 }, border { 0xff2a2f3a }, accent { 0xffff9a2e }, text_ { 0xffe8eaf0 }, muted { 0xff8b93a7 };
    };

    juce::AudioProcessorEditor* CompPlugin::createEditor() { return new CompEditor (*this); }

    // ------------------------------------------------------------ noise gate (zero latency)
    // No look-ahead: the gate opens as fast as the Attack time allows (min 0.05 ms) and closes after Hold + Release.
    // The detector can be filtered (Low Cut / High Cut) so that rumble or hiss does not open the gate (e.g. stage bleed).
    class GatePlugin : public BuiltinBase
    {
    public:
        GatePlugin() : BuiltinBase ("gate", "Gate", "Noise gate (zero latency)")
        {
            thresh = addLinear ("th", "Threshold", -80.0f, 0.0f, -45.0f);
            range = addLinear ("rg", "Range", -100.0f, 0.0f, -100.0f);
            attack = addFloat ("at", "Attack", 0.05f, 50.0f, 0.5f, 0.1f);
            hold = addFloat ("ho", "Hold", 0.0f, 1000.0f, 80.0f, 0.15f);
            release = addFloat ("re", "Release", 5.0f, 3000.0f, 150.0f, 0.1f);
            hyst = addLinear ("hy", "Hysteresis", 0.0f, 20.0f, 4.0f);
            lowCut = addFloat ("lc", "Low Cut", 20.0f, 2000.0f, 20.0f, 0.1f);
            highCut = addFloat ("hc", "High Cut", 500.0f, 20000.0f, 20000.0f, 0.3f);
            ratio = addFloat ("ra", "Ratio", 1.5f, 100.0f, 10.0f, 0.1f);
            on = new juce::AudioParameterBool (juce::ParameterID { "on", 1 }, "On", true);
            juce::AudioProcessor::addParameter (on);
        }

        void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buf.getNumSamples(), nch = juce::jmin (2, buf.getNumChannels());
            if (nch == 0)
                return;

            if (! on->get())
            {
                gain = 1.0f;
                inLevel.store (0.0f);
                reduction.store (0.0f);
                isOpen.store (true);
                return;
            }

            const float th = thresh->get(), hy = hyst->get(), rangeDb = range->get(), expand = ratio->get() - 1.0f;
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

        juce::AudioParameterFloat *thresh, *range, *attack, *hold, *release, *hyst, *lowCut, *highCut, *ratio;
        juce::AudioParameterBool* on;
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
            setSize (760, 360);
            addRow (atS, atL, "ATTACK", *p.attack, atA);
            addRow (hoS, hoL, "HOLD", *p.hold, hoA);
            addRow (reS, reL, "RELEASE", *p.release, reA);
            addRow (thS, thL, "THRESHOLD", *p.thresh, thA);
            addRow (raS, raL, "RATIO", *p.ratio, raA);
            addRow (rgS, rgL, "RANGE", *p.range, rgA);
            addRow (hyS, hyL, "HYSTERESIS", *p.hyst, hyA);
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
            hyS.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " dB"; };
            lcS.textFromValueFunction = hz;
            hcS.textFromValueFunction = hz;
            for (auto* s : { &thS, &rgS, &atS, &hoS, &reS, &hyS, &lcS, &hcS, &raS })
                s->updateText();

            onBtn.setButtonText ("ON");
            onBtn.setClickingTogglesState (true);
            onBtn.setColour (juce::TextButton::buttonColourId, bg2);
            onBtn.setColour (juce::TextButton::buttonOnColourId, accent);
            onBtn.setColour (juce::TextButton::textColourOffId, text_);
            onBtn.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff15171c));
            addAndMakeVisible (onBtn);
            onA = std::make_unique<juce::ButtonParameterAttachment> (*p.on, onBtn);

            startTimerHz (60);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (bg);
            g.setColour (text_);
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText ("GATE", 90, 8, 120, 28, juce::Justification::centredLeft);

            const bool o = plug.isOpen.load() && plug.on->get();
            g.setColour (o ? juce::Colour (0xff4cd964) : juce::Colour (0xff3a4152));
            g.fillEllipse (200.0f, 16.0f, 12.0f, 12.0f);
            g.setColour (muted);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (o ? "OPEN" : "CLOSED", 218, 8, 70, 28, juce::Justification::centredLeft);

            paintTransfer (g);
        }

        void resized() override
        {
            onBtn.setBounds (16, 8, 64, 28);
            transferArea = juce::Rectangle<int> (16, 48, 340, 290);

            juce::Slider* sl[9] = { &thS, &raS, &rgS, &atS, &hoS, &reS, &hyS, &lcS, &hcS };
            juce::Label* lb[9] = { &thL, &raL, &rgL, &atL, &hoL, &reL, &hyL, &lcL, &hcL };
            for (int i = 0; i < 9; ++i)
            {
                auto row = juce::Rectangle<int> (372, 48 + i * 32, 372, 32);
                lb[i]->setBounds (row.removeFromLeft (104));
                sl[i]->setBounds (row);
            }
        }

    private:
        void paintTransfer (juce::Graphics& g)
        {
            auto plot = juce::Rectangle<float> ((float) transferArea.getX() + 84.0f, (float) transferArea.getY() + 4.0f, 252.0f, 252.0f);
            g.setColour (juce::Colour (0xff0d0f13));
            g.fillRect (transferArea.toFloat());

            auto X = [&] (float db) { return plot.getX() + (juce::jlimit (-100.0f, 0.0f, db) + 100.0f) / 100.0f * plot.getWidth(); };
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

            // threshold marker
            g.setColour (juce::Colour (0x80ff9a2e));
            g.drawVerticalLine ((int) X (th), plot.getY(), plot.getBottom());

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
            repaint (180, 8, 120, 28);
        }

        GatePlugin& plug;
        juce::Slider thS, rgS, atS, hoS, reS, hyS, lcS, hcS, raS;
        juce::Label thL, rgL, atL, hoL, reL, hyL, lcL, hcL, raL;
        std::unique_ptr<juce::SliderParameterAttachment> thA, rgA, atA, hoA, reA, hyA, lcA, hcA, raA;
        juce::TextButton onBtn;
        std::unique_ptr<juce::ButtonParameterAttachment> onA;
        juce::Rectangle<int> transferArea;
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

            // LED and foot switch
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

    // ------------------------------------------------------------ guitar amp + cabinet (zero latency, see AmpCore.h)
    class AmpPlugin : public BuiltinBase
    {
    public:
        AmpPlugin() : BuiltinBase ("amp", "Amp", "Guitar amplifier and cabinet (zero latency)")
        {
            model = new juce::AudioParameterChoice (juce::ParameterID { "md", 1 }, "Channel", juce::StringArray { "Clean", "Crunch", "Lead" }, 1);
            juce::AudioProcessor::addParameter (model);
            cab = new juce::AudioParameterChoice (juce::ParameterID { "cb", 1 }, "Cabinet", juce::StringArray { "Off", "1x12", "2x12", "4x12" }, 3);
            juce::AudioProcessor::addParameter (cab);
            input = addLinear ("in", "Input", -12.0f, 12.0f, 0.0f);
            gain = addLinear ("ga", "Gain", 0.0f, 100.0f, 50.0f);
            bass = addLinear ("ba", "Bass", 0.0f, 100.0f, 50.0f);
            mid = addLinear ("mi", "Mid", 0.0f, 100.0f, 50.0f);
            treble = addLinear ("tr", "Treble", 0.0f, 100.0f, 50.0f);
            presence = addLinear ("pr", "Presence", 0.0f, 100.0f, 50.0f);
            master = addLinear ("ma", "Master", -40.0f, 6.0f, -6.0f);
        }

        void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buf.getNumSamples(), nch = juce::jmin (2, buf.getNumChannels());
            tcamp::Params p;
            p.model = model->getIndex();
            p.cab = cab->getIndex();
            p.inputDb = input->get();
            p.gain = gain->get() * 0.01f;
            p.bass = bass->get() * 0.01f;
            p.mid = mid->get() * 0.01f;
            p.treble = treble->get() * 0.01f;
            p.presence = presence->get() * 0.01f;
            p.masterDb = master->get();
            if (! haveParams || std::memcmp (&p, &last, sizeof (p)) != 0)
            {
                last = p;
                haveParams = true;
                for (auto& c : ch)
                    c.setParams (p);
            }
            for (int c = 0; c < nch; ++c)
            {
                auto* d = buf.getWritePointer (c);
                for (int i = 0; i < n; ++i)
                    d[i] = ch[c].process (d[i]);
            }
        }

        juce::AudioParameterChoice *model, *cab;
        juce::AudioParameterFloat *input, *gain, *bass, *mid, *treble, *presence, *master;
        juce::AudioProcessorEditor* createEditor() override;

    protected:
        void prepare() override
        {
            haveParams = false;
            for (auto& c : ch)
                c.prepare (sr);
        }

    private:
        tcamp::Channel ch[2];
        tcamp::Params last;
        bool haveParams = false;
    };

    class AmpEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit AmpEditor (AmpPlugin& p) : juce::AudioProcessorEditor (p), plug (p)
        {
            setSize (720, 350);
            addKnob (inS, inL, "INPUT", *p.input, inA);
            addKnob (gaS, gaL, "GAIN", *p.gain, gaA);
            addKnob (baS, baL, "BASS", *p.bass, baA);
            addKnob (miS, miL, "MID", *p.mid, miA);
            addKnob (trS, trL, "TREBLE", *p.treble, trA);
            addKnob (prS, prL, "PRESENCE", *p.presence, prA);
            addKnob (maS, maL, "MASTER", *p.master, maA);
            auto tenth = [] (double v) { return juce::String (v / 10.0, 1); };
            for (auto* s : { &gaS, &baS, &miS, &trS, &prS })
                s->textFromValueFunction = tenth;
            inS.textFromValueFunction = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; };
            maS.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " dB"; };
            for (auto* s : { &inS, &gaS, &baS, &miS, &trS, &prS, &maS })
                s->updateText();

            const char* modelNames[3] = { "CLEAN", "CRUNCH", "LEAD" };
            const char* cabNames[4] = { "NO CAB", "1x12", "2x12", "4x12" };
            for (int i = 0; i < 3; ++i)
                setupButton (modelBtn[i], modelNames[i], [this, i] { plug.model->beginChangeGesture(); *plug.model = i; plug.model->endChangeGesture(); });
            for (int i = 0; i < 4; ++i)
                setupButton (cabBtn[i], cabNames[i], [this, i] { plug.cab->beginChangeGesture(); *plug.cab = i; plug.cab->endChangeGesture(); });
            startTimerHz (15);
            timerCallback();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (bg);
            auto body = getLocalBounds().toFloat().reduced (10.0f);
            juce::ColourGradient grad (juce::Colour (0xff23262e), body.getX(), body.getY(), juce::Colour (0xff181a20), body.getX(), body.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (body, 10.0f);
            g.setColour (juce::Colour (0xff3a4152));
            g.drawRoundedRectangle (body, 10.0f, 2.0f);
            g.setColour (accent);
            g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
            g.drawText ("AMP", 24, 16, 120, 30, juce::Justification::centredLeft);
            g.setColour (muted);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("CHANNEL", 150, 20, 60, 14, juce::Justification::centredLeft);
            g.drawText ("CABINET", 24, getHeight() - 52, 70, 14, juce::Justification::centredLeft);
        }

        void resized() override
        {
            for (int i = 0; i < 3; ++i)
                modelBtn[i].setBounds (214 + i * 84, 16, 78, 28);
            const int w = (getWidth() - 48) / 7;
            juce::Slider* sl[7] = { &inS, &gaS, &baS, &miS, &trS, &prS, &maS };
            juce::Label* lb[7] = { &inL, &gaL, &baL, &miL, &trL, &prL, &maL };
            for (int i = 0; i < 7; ++i)
            {
                auto c = juce::Rectangle<int> (24 + i * w, 60, w, 190);
                lb[i]->setBounds (c.removeFromTop (20));
                sl[i]->setBounds (c);
            }
            for (int i = 0; i < 4; ++i)
                cabBtn[i].setBounds (100 + i * 84, getHeight() - 58, 78, 28);
        }

    private:
        template <typename F>
        void setupButton (juce::TextButton& b, const char* text, F&& onClick)
        {
            b.setButtonText (text);
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff262b36));
            b.setColour (juce::TextButton::buttonOnColourId, accent);
            b.setColour (juce::TextButton::textColourOffId, text_);
            b.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff15171c));
            b.onClick = std::forward<F> (onClick);
            addAndMakeVisible (b);
        }

        void addKnob (juce::Slider& s, juce::Label& l, const juce::String& text, juce::RangedAudioParameter& prm, std::unique_ptr<juce::SliderParameterAttachment>& att)
        {
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 20);
            s.setColour (juce::Slider::rotarySliderFillColourId, accent);
            s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3a4152));
            s.setColour (juce::Slider::thumbColourId, accent);
            s.setColour (juce::Slider::textBoxTextColourId, text_);
            s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            addAndMakeVisible (s);
            l.setText (text, juce::dontSendNotification);
            l.setJustificationType (juce::Justification::centred);
            l.setColour (juce::Label::textColourId, muted);
            l.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            addAndMakeVisible (l);
            att = std::make_unique<juce::SliderParameterAttachment> (prm, s);
        }

        void timerCallback() override
        {
            for (int i = 0; i < 3; ++i)
                modelBtn[i].setToggleState (plug.model->getIndex() == i, juce::dontSendNotification);
            for (int i = 0; i < 4; ++i)
                cabBtn[i].setToggleState (plug.cab->getIndex() == i, juce::dontSendNotification);
        }

        AmpPlugin& plug;
        juce::Slider inS, gaS, baS, miS, trS, prS, maS;
        juce::Label inL, gaL, baL, miL, trL, prL, maL;
        std::unique_ptr<juce::SliderParameterAttachment> inA, gaA, baA, miA, trA, prA, maA;
        juce::TextButton modelBtn[3], cabBtn[4];
        juce::Colour bg { 0xff15171c }, accent { 0xffff9a2e }, text_ { 0xffe8eaf0 }, muted { 0xff8b93a7 };
    };

    juce::AudioProcessorEditor* AmpPlugin::createEditor() { return new AmpEditor (*this); }


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
            voicesP = addLinear ("vo", "Voices", 0.0f, 6.0f, 0.0f);
            spreadP = addLinear ("sp", "Spread", 0.0f, 100.0f, 40.0f);
            detune = addLinear ("de", "Detune", 430.0f, 450.0f, 440.0f);
            classic = new juce::AudioParameterBool (juce::ParameterID { "cl", 1 }, "Classic", false);
            hq = new juce::AudioParameterBool (juce::ParameterID { "hq", 1 }, "HQ", false);
            on = new juce::AudioParameterBool (juce::ParameterID { "on", 1 }, "On", true);
            range = new juce::AudioParameterChoice (juce::ParameterID { "ra", 1 }, "Vocal Range",
                                                    juce::StringArray { "Soprano", "Alto/Tenor", "Low Male", "Instrument" }, 1);
            key = new juce::AudioParameterInt (juce::ParameterID { "ky", 1 }, "Key", 0, 48, 0);
            for (auto* p : { (juce::AudioProcessorParameter*) classic, (juce::AudioProcessorParameter*) hq, (juce::AudioProcessorParameter*) on,
                             (juce::AudioProcessorParameter*) range, (juce::AudioProcessorParameter*) key })
                juce::AudioProcessor::addParameter (p);
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
            p.voices = (int) std::lround (voicesP->get());
            p.spread = spreadP->get();
            std::uint16_t m = 0;
            for (int i = 0; i < 12; ++i)
                if (note[i]->get())
                    m |= (std::uint16_t) (1u << i);
            p.mask = m;
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

        juce::AudioParameterFloat *retune, *human, *flex, *voicesP, *spreadP, *detune;
        juce::AudioParameterBool *classic, *hq, *on, *note[12];
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
            setSize (860, 520);

            knob (rtS, "Retune Speed", *p.retune, rtA, teal, 0);
            knob (huS, "Humanize", *p.human, huA, juce::Colour (0xff8a63d2), 0);
            knob (fxS, "Flex Tune", *p.flex, fxA, juce::Colour (0xffe6d94a), 0);
            knob (voS, "Voices", *p.voicesP, voA, juce::Colour (0xff4fb3ff), 0);
            knob (spS, "Spread", *p.spreadP, spA, juce::Colour (0xffff9a4d), 0);
            voS.setNumDecimalPlacesToDisplay (0);
            voS.setRange (0.0, 6.0, 1.0);

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
            g.drawText (letter, 190, 74, 180, 120, juce::Justification::centred);
            if (sharp)
            {
                g.setFont (juce::FontOptions (40.0f));
                g.drawText (juce::String::fromUTF8 ("\xE2\x99\xAF"), 330, 78, 50, 50, juce::Justification::centred);
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
            for (int i = 0; i < 12; ++i)
            {
                auto k = keyRect (i).toFloat();
                const bool allowed = plug.note[i]->get();
                const bool hot = voiced && target == i;
                g.setColour (hot ? teal.withAlpha (0.9f) : allowed ? teal.withAlpha (0.18f) : juce::Colour (0xff161c1f));
                g.fillRoundedRectangle (k, k.getWidth() * 0.4f);
                g.setColour (allowed ? teal : juce::Colour (0xff2a3338));
                g.drawRoundedRectangle (k.reduced (0.5f), k.getWidth() * 0.4f, 1.5f);
                g.setColour (hot ? juce::Colours::black : allowed ? juce::Colours::white : juce::Colour (0xff5b676d));
                g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
                g.drawText (tunekeys::noteNames[i], k.toNearestInt(), juce::Justification::centred);
            }

            // bottom of the left panel
            g.setColour (muted);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("Detune", detuneS.getX(), detuneS.getY() - 14, 80, 12, juce::Justification::centredLeft);
            g.drawText ("Mode", modeLL.getX(), modeLL.getY() - 14, 80, 12, juce::Justification::centredLeft);
            g.setColour (juce::Colour (0xff2a3338));
            g.drawLine (30.0f, 408.0f, left.getRight() - 20.0f, 408.0f);

            // right panel
            for (auto* s : { &rtS, &huS, &fxS, &voS, &spS })
            {
                g.setColour (muted.brighter (0.4f));
                g.setFont (juce::FontOptions (s == &rtS ? 16.0f : 13.0f));
                g.drawText (s->getName(), s->getX() - 20, s->getY() - (s == &rtS ? 22 : 18), s->getWidth() + 40, 18, juce::Justification::centred);
            }
            // power
            auto pw = powerRect().toFloat();
            const bool o = plug.on->get();
            g.setColour (o ? teal : juce::Colour (0xff2a3338));
            g.fillEllipse (pw);
            g.setColour (o ? juce::Colours::black : muted);
            juce::Path arc;
            arc.addCentredArc (pw.getCentreX(), pw.getCentreY(), 8.0f, 8.0f, 0.0f, 0.6f, 6.28f - 0.6f, true);
            g.strokePath (arc, juce::PathStrokeType (2.0f));
            g.drawLine (pw.getCentreX(), pw.getCentreY() - 11.0f, pw.getCentreX(), pw.getCentreY() - 2.0f, 2.0f);
        }

        void resized() override
        {
            rangeBtn.setBounds (260, 36, 130, 28);
            keyBtn.setBounds (404, 36, 150, 28);
            detuneS.setBounds (40, 436, 110, 28);
            modeLL.setBounds (240, 436, 110, 26);
            modeHQ.setBounds (354, 436, 60, 26);

            auto r = rightPanel();
            rtS.setBounds (r.getX() + 60, 70, 170, 170);
            huS.setBounds (r.getX() + 28, 296, 84, 84);
            fxS.setBounds (r.getX() + 130, 296, 84, 84);
            voS.setBounds (r.getX() + 28, 410, 84, 84);
            spS.setBounds (r.getX() + 130, 410, 84, 84);
            styleModern.setBounds (r.getX() + 20, 250, 98, 26);
            styleClassic.setBounds (r.getX() + 126, 250, 98, 26);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            for (int i = 0; i < 12; ++i)
                if (keyRect (i).contains (e.position))
                {
                    plug.note[i]->beginChangeGesture();
                    *plug.note[i] = ! plug.note[i]->get();
                    plug.note[i]->endChangeGesture();
                    return;
                }
            if (resetRect().contains (e.position))
                setKey (plug.key->get());
            if (powerRect().toFloat().contains (e.position))
                setBool (*plug.on, ! plug.on->get());
        }

    private:
        juce::Rectangle<int> leftPanel() const { return { 14, 14, 590, getHeight() - 28 }; }
        juce::Rectangle<int> rightPanel() const { return { 616, 14, 230, getHeight() - 28 }; }
        juce::Rectangle<int> meterRect() const { return { 44, 214, 520, 30 }; }
        juce::Rectangle<float> keyRect (int i) const
        {
            // white keys C D E F G A B on the lower row, black keys between them on the upper row
            static const int white[7] = { 0, 2, 4, 5, 7, 9, 11 };
            const float w = 62.0f, h = 62.0f, gap = 8.0f, x0 = 62.0f;
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
        juce::Rectangle<float> resetRect() const { return { 540.0f, 284.0f, 24.0f, 22.0f }; }
        juce::Rectangle<int> powerRect() const { return { rightPanel().getRight() - 62, getHeight() - 70, 36, 36 }; }

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
            keyBtn.setButtonText (name);

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
        juce::Slider rtS, huS, fxS, voS, spS, detuneS;
        std::unique_ptr<juce::SliderParameterAttachment> rtA, huA, fxA, voA, spA, detuneA;
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
        return nullptr;
    }

    const char* const allIds[] = { "TC:eq", "TC:comp", "TC:gate", "TC:reverb", "TC:od808", "TC:amp", "TC:tune" };
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
