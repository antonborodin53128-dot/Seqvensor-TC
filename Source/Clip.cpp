#include "Clip.h"
#include "Text.h"
#include "Realtime.h"

#include <limits>
#include <new>

std::shared_ptr<ClipData> ClipData::open (const juce::File& file, juce::String& error,
                                          std::unique_ptr<juce::AudioFormatReader>& readerOut)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    readerOut.reset (formats.createReaderFor (file));
    if (readerOut == nullptr)
    {
        error = "Не удалось открыть файл"_ru;
        return nullptr;
    }

    const juce::int64 total = readerOut->lengthInSamples;
    const int channels = (int) juce::jlimit<juce::uint32> (1, 2, readerOut->numChannels);
    if (total <= 0)
    {
        error = "Файл пуст"_ru;
        readerOut.reset();
        return nullptr;
    }

    // The whole file is decoded into memory (instant seeks, no disk on stage). The limit is half of the memory that is
    // free right now (at least 1.6 GB), and 2^31 samples (12 h at 48 kHz) per file.
    const juce::int64 need = total * channels * (juce::int64) sizeof (float);
    const juce::int64 avail = tcrt::availableMemoryBytes();
    const juce::int64 limit = juce::jmax ((juce::int64) 1600000000LL, avail / 2);
    if (need > limit || total >= (juce::int64) std::numeric_limits<int>::max() - 1)
    {
        const double gb = 1024.0 * 1024.0 * 1024.0;
        error = total >= (juce::int64) std::numeric_limits<int>::max() - 1
                  ? "Файл длиннее 12 часов"_ru
                  : "Файлу нужно "_ru + juce::String ((double) need / gb, 1) + " ГБ памяти, свободно "_ru
                        + juce::String ((double) avail / gb, 1) + " ГБ. Закройте другие программы или разрежьте файл."_ru;
        readerOut.reset();
        return nullptr;
    }

    auto d = std::make_shared<ClipData>();
    d->file = file;
    d->name = file.getFileNameWithoutExtension();
    d->sampleRate = readerOut->sampleRate;
    d->numSamples = (int) total;
    try
    {
        d->audio.setSize (channels, d->numSamples);
        d->audio.clear();
        d->peaks.assign ((size_t) ((d->numSamples + peakBlock - 1) / peakBlock) * 2, 0.0f);
    }
    catch (const std::bad_alloc&)
    {
        error = "Не хватает памяти для файла"_ru;
        readerOut.reset();
        return nullptr;
    }
    d->decoded.store (0);
    return d;
}

void ClipData::fill (juce::AudioFormatReader& reader, const std::function<void()>& progress)
{
    const int channels = audio.getNumChannels();
    constexpr int chunk = 262144;   // a multiple of peakBlock
    for (int pos = 0; pos < numSamples; pos += chunk)
    {
        const int n = juce::jmin (chunk, numSamples - pos);
        reader.read (&audio, pos, n, pos, true, channels > 1);

        const float* l = audio.getReadPointer (0);
        const float* r = channels > 1 ? audio.getReadPointer (1) : nullptr;
        for (int b = pos / peakBlock; b * peakBlock < pos + n; ++b)
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
        decoded.store (pos + n);
        if (progress)
            progress();
    }
    decoded.store (numSamples);
}

std::shared_ptr<ClipData> ClipData::load (const juce::File& file, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader;
    auto d = open (file, error, reader);
    if (d != nullptr)
        d->fill (*reader, {});
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
