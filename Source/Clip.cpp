#include "Clip.h"
#include "Text.h"

std::shared_ptr<ClipData> ClipData::load (const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Не удалось открыть файл"_ru;
        return nullptr;
    }

    const juce::int64 total = reader->lengthInSamples;
    const int channels = (int) juce::jlimit<juce::uint32> (1, 2, reader->numChannels);
    if (total <= 0 || total * channels * 4 > 1600000000LL)
    {
        error = total <= 0 ? "Файл пуст"_ru : "Файл слишком большой"_ru;
        return nullptr;
    }

    auto d = std::make_shared<ClipData>();
    d->file = file;
    d->name = file.getFileNameWithoutExtension();
    d->sampleRate = reader->sampleRate;
    d->numSamples = (int) total;
    d->audio.setSize (channels, d->numSamples);

    constexpr int chunk = 262144;
    for (juce::int64 pos = 0; pos < total; pos += chunk)
    {
        const int n = (int) juce::jmin ((juce::int64) chunk, total - pos);
        reader->read (&d->audio, (int) pos, n, pos, true, channels > 1);
    }

    d->buildPeaks();
    return d;
}

void ClipData::buildPeaks()
{
    const int blocks = (numSamples + peakBlock - 1) / peakBlock;
    peaks.assign ((size_t) blocks * 2, 0.0f);
    const float* l = audio.getReadPointer (0);
    const float* r = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : nullptr;
    for (int b = 0; b < blocks; ++b)
    {
        float lo = 1.0e9f, hi = -1.0e9f;
        const int s0 = b * peakBlock, s1 = juce::jmin (numSamples, s0 + peakBlock);
        for (int i = s0; i < s1; ++i)
        {
            const float v = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
            lo = juce::jmin (lo, v);
            hi = juce::jmax (hi, v);
        }
        peaks[(size_t) b * 2] = lo;
        peaks[(size_t) b * 2 + 1] = hi;
    }
}

void ClipData::draw (juce::Graphics& g, juce::Rectangle<int> area, double t0, double t1) const
{
    if (area.isEmpty() || t1 <= t0 || numSamples <= 0)
        return;

    const float mid = (float) area.getCentreY();
    const float half = (float) area.getHeight() * 0.5f - 1.0f;
    const double secPerPx = (t1 - t0) / (double) area.getWidth();
    const float* l = audio.getReadPointer (0);
    const float* r = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : nullptr;

    for (int px = 0; px < area.getWidth(); ++px)
    {
        const double a = t0 + px * secPerPx, b = a + secPerPx;
        const int s0 = juce::jlimit (0, numSamples - 1, (int) std::floor (a * sampleRate));
        const int s1 = juce::jlimit (s0 + 1, numSamples, (int) std::ceil (b * sampleRate));

        float lo = 1.0e9f, hi = -1.0e9f;
        if (s1 - s0 >= peakBlock * 2)
        {
            const int b0 = s0 / peakBlock, b1 = juce::jmin ((int) peaks.size() / 2 - 1, s1 / peakBlock);
            for (int k = b0; k <= b1; ++k)
            {
                lo = juce::jmin (lo, peaks[(size_t) k * 2]);
                hi = juce::jmax (hi, peaks[(size_t) k * 2 + 1]);
            }
        }
        else
        {
            for (int i = s0; i < s1; ++i)
            {
                const float v = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
                lo = juce::jmin (lo, v);
                hi = juce::jmax (hi, v);
            }
        }

        const int y0 = (int) std::floor (mid - juce::jlimit (-1.0f, 1.0f, hi) * half);
        const int y1 = (int) std::ceil (mid - juce::jlimit (-1.0f, 1.0f, lo) * half);
        g.fillRect (area.getX() + px, y0, 1, juce::jmax (1, y1 - y0));
    }
}

void ClipData::getMono (std::vector<float>& out, int maxSamples) const
{
    const int n = juce::jmin (numSamples, maxSamples);
    out.resize ((size_t) n);
    const float* l = audio.getReadPointer (0);
    const float* r = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : nullptr;
    for (int i = 0; i < n; ++i)
        out[(size_t) i] = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
}
