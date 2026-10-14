#include "Builtin.h"
#include "Text.h"

#include <cmath>

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

    // ------------------------------------------------------------ equaliser: high-pass, low shelf, two bells, high shelf
    class EqPlugin : public BuiltinBase
    {
    public:
        EqPlugin() : BuiltinBase ("eq", "Эквалайзер"_ru, "Эквалайзер: ФВЧ, 2 полки, 2 колокола"_ru)
        {
            hp    = addFloat ("hp",   "ФВЧ, Гц"_ru,            20.0f, 500.0f, 20.0f, 0.15f);
            lsF   = addFloat ("lsf",  "НЧ полка, Гц"_ru,       30.0f, 500.0f, 120.0f, 0.3f);
            lsG   = addLinear ("lsg", "НЧ полка, дБ"_ru,      -18.0f, 18.0f, 0.0f);
            m1F   = addFloat ("m1f",  "Колокол 1, Гц"_ru,     100.0f, 3000.0f, 400.0f, 0.3f);
            m1G   = addLinear ("m1g", "Колокол 1, дБ"_ru,     -18.0f, 18.0f, 0.0f);
            m1Q   = addFloat ("m1q",  "Колокол 1, добротность"_ru, 0.3f, 8.0f, 1.0f, 0.3f);
            m2F   = addFloat ("m2f",  "Колокол 2, Гц"_ru,     500.0f, 10000.0f, 3000.0f, 0.3f);
            m2G   = addLinear ("m2g", "Колокол 2, дБ"_ru,     -18.0f, 18.0f, 0.0f);
            m2Q   = addFloat ("m2q",  "Колокол 2, добротность"_ru, 0.3f, 8.0f, 1.0f, 0.3f);
            hsF   = addFloat ("hsf",  "ВЧ полка, Гц"_ru,     2000.0f, 18000.0f, 8000.0f, 0.3f);
            hsG   = addLinear ("hsg", "ВЧ полка, дБ"_ru,     -18.0f, 18.0f, 0.0f);
            level = addLinear ("out", "Выход, дБ"_ru,        -18.0f, 18.0f, 0.0f);
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            const int channels = juce::jmin (2, buffer.getNumChannels());
            const int n = buffer.getNumSamples();

            // coefficients are recomputed once per block: cheap, and the sound follows the knobs
            const bool useHp = hp->get() > 21.0f;
            if (useHp) hpF.makeHighPass (sr, hp->get());
            const bool useLs = std::abs (lsG->get()) > 0.05f, useM1 = std::abs (m1G->get()) > 0.05f,
                       useM2 = std::abs (m2G->get()) > 0.05f, useHs = std::abs (hsG->get()) > 0.05f;
            if (useLs) ls.makeLowShelf (sr, lsF->get(), lsG->get());
            if (useM1) m1.makePeak (sr, m1F->get(), m1G->get(), m1Q->get());
            if (useM2) m2.makePeak (sr, m2F->get(), m2G->get(), m2Q->get());
            if (useHs) hs.makeHighShelf (sr, juce::jmin (hsF->get(), (float) sr * 0.45f), hsG->get());
            const float gain = std::pow (10.0f, level->get() / 20.0f);

            for (int c = 0; c < channels; ++c)
            {
                float* d = buffer.getWritePointer (c);
                for (int i = 0; i < n; ++i)
                {
                    float x = d[i];
                    if (useHp) x = hpF.process (c, x);
                    if (useLs) x = ls.process (c, x);
                    if (useM1) x = m1.process (c, x);
                    if (useM2) x = m2.process (c, x);
                    if (useHs) x = hs.process (c, x);
                    d[i] = x * gain;
                }
            }
        }

    protected:
        void prepare() override
        {
            for (auto* b : { &hpF, &ls, &m1, &m2, &hs })
                b->reset();
        }

    private:
        juce::AudioParameterFloat *hp, *lsF, *lsG, *m1F, *m1G, *m1Q, *m2F, *m2G, *m2Q, *hsF, *hsG, *level;
        Biquad hpF, ls, m1, m2, hs;
    };

    std::unique_ptr<juce::AudioPluginInstance> makeBuiltin (const juce::String& id)
    {
        if (id == "TC:eq")
            return std::make_unique<EqPlugin>();
        return nullptr;
    }

    const char* const allIds[] = { "TC:eq" };
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
