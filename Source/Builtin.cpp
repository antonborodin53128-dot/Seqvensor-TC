#include "Builtin.h"
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
            auto c4 = row2.reduced (10, 20);
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

    std::unique_ptr<juce::AudioPluginInstance> makeBuiltin (const juce::String& id)
    {
        if (id == "TC:eq")
            return std::make_unique<EqPlugin>();
        if (id == "TC:comp")
            return std::make_unique<CompPlugin>();
        return nullptr;
    }

    const char* const allIds[] = { "TC:eq", "TC:comp" };
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
