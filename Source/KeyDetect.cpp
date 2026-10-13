#include "KeyDetect.h"
#include "Text.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <mutex>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <onnxruntime_cxx_api.h>
 #include "EmbeddedIds.h"
#endif

namespace
{
    constexpr int modelRate = 22050;
    constexpr int windowLen = 43844;                   // 2 s of audio the network takes at once
    constexpr int overlapFrames = 30;
    constexpr int fftHop = 256;
    constexpr int overlapLen = overlapFrames * fftHop;
    constexpr int hopLen = windowLen - overlapLen;
    constexpr int framesPerWindow = 172;
    constexpr int numPitches = 88;                     // piano keys, A0 (MIDI 21) .. C8
    constexpr float noteThreshold = 0.25f;

    juce::String lastError;

   #if JUCE_WINDOWS
    // The ONNX Runtime DLL and the model are inside the exe (resources). The DLL is unpacked next to the settings
    // once and loaded from there; the import of onnxruntime.dll is delay-loaded, so the exe starts without it.
    bool getResource (int id, const void*& data, size_t& size)
    {
        HRSRC res = FindResourceW (nullptr, MAKEINTRESOURCEW (id), MAKEINTRESOURCEW (10));   // 10 = RT_RCDATA
        if (res == nullptr)
            return false;
        HGLOBAL h = LoadResource (nullptr, res);
        if (h == nullptr)
            return false;
        data = LockResource (h);
        size = (size_t) SizeofResource (nullptr, res);
        return data != nullptr && size > 0;
    }

    struct Net
    {
        Ort::Env env { ORT_LOGGING_LEVEL_ERROR, "tc" };
        Ort::Session session { nullptr };
        std::mutex runLock;
        bool ok = false;

        Net()
        {
            try
            {
                const void* dll = nullptr; size_t dllSize = 0;
                const void* model = nullptr; size_t modelSize = 0;
                if (! getResource (IDR_ORT_DLL, dll, dllSize) || ! getResource (IDR_MODEL, model, modelSize))
                {
                    lastError = "нет встроенных ресурсов"_ru;
                    return;
                }

                auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                               .getChildFile ("TC").getChildFile ("ort");
                dir.createDirectory();
                auto file = dir.getChildFile ("onnxruntime.dll");
                if (! file.existsAsFile() || (size_t) file.getSize() != dllSize)
                {
                    juce::TemporaryFile tmp (file);
                    if (! tmp.getFile().replaceWithData (dll, dllSize) || ! tmp.overwriteTargetFileWithTemporary())
                    {
                        lastError = "не удалось распаковать onnxruntime.dll"_ru;
                        return;
                    }
                }

                if (LoadLibraryExW (file.getFullPathName().toWideCharPointer(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr)
                {
                    lastError = "не удалось загрузить onnxruntime.dll, код "_ru + juce::String ((int) GetLastError());
                    return;
                }

                Ort::SessionOptions opts;
                opts.SetIntraOpNumThreads (2);
                opts.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
                session = Ort::Session (env, model, modelSize, opts);
                ok = true;
            }
            catch (const std::exception& e)
            {
                lastError = juce::String::fromUTF8 (e.what());
            }
        }
    };

    Net& net()
    {
        static Net n;   // first use loads the runtime
        return n;
    }

    // note probabilities of one 2 s window: frames x 88
    bool runWindow (Net& n, const float* audio, std::vector<float>& notes)
    {
        try
        {
            static const char* inName = "serving_default_input_2:0";
            static const char* outName = "StatefulPartitionedCall:1";
            const int64_t shape[3] = { 1, windowLen, 1 };
            auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
            auto input = Ort::Value::CreateTensor<float> (mem, const_cast<float*> (audio), (size_t) windowLen, shape, 3);

            std::lock_guard<std::mutex> sl (n.runLock);
            auto out = n.session.Run (Ort::RunOptions { nullptr }, &inName, &input, 1, &outName, 1);
            const float* p = out[0].GetTensorData<float>();
            const size_t count = out[0].GetTensorTypeAndShapeInfo().GetElementCount();
            if (count < (size_t) framesPerWindow * numPitches)
                return false;
            notes.assign (p, p + (size_t) framesPerWindow * numPitches);
            return true;
        }
        catch (const std::exception& e)
        {
            lastError = juce::String::fromUTF8 (e.what());
            return false;
        }
    }
   #endif

    // 22.05 kHz mono for the network: low-pass, then linear interpolation (the network only needs < 4 kHz)
    std::vector<float> toModelRate (const std::vector<float>& in, double sr)
    {
        if (in.empty() || sr <= 0.0)
            return {};

        std::vector<float> filtered (in);
        if (sr > modelRate * 1.01)
        {
            juce::IIRFilter lp1, lp2;
            const auto coeffs = juce::IIRCoefficients::makeLowPass (sr, 8500.0);
            lp1.setCoefficients (coeffs);
            lp2.setCoefficients (coeffs);
            lp1.processSamples (filtered.data(), (int) filtered.size());
            lp2.processSamples (filtered.data(), (int) filtered.size());
        }

        const double step = sr / modelRate;
        const size_t n = (size_t) std::floor ((double) filtered.size() / step);
        std::vector<float> out (n);
        for (size_t i = 0; i < n; ++i)
        {
            const double pos = (double) i * step;
            const size_t k = (size_t) pos;
            const float f = (float) (pos - (double) k);
            const float a = filtered[k];
            const float b = k + 1 < filtered.size() ? filtered[k + 1] : a;
            out[i] = a + (b - a) * f;
        }
        return out;
    }

    const double majorProfile[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    const double minorProfile[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    double correlate (const double* x, const double* profile, int shift)
    {
        double mx = 0.0, my = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            mx += x[i];
            my += profile[((i - shift) % 12 + 12) % 12];
        }
        mx /= 12.0;
        my /= 12.0;
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            const double a = x[i] - mx;
            const double b = profile[((i - shift) % 12 + 12) % 12] - my;
            sxy += a * b;
            sxx += a * a;
            syy += b * b;
        }
        return (sxx > 0.0 && syy > 0.0) ? sxy / std::sqrt (sxx * syy) : 0.0;
    }
}

KeyHist computeKeyHist (const std::vector<float>& mono, double sampleRate)
{
    KeyHist result;
   #if JUCE_WINDOWS
    Net& n = net();
    if (! n.ok)
        return result;

    std::vector<float> audio = toModelRate (mono, sampleRate);
    if (audio.size() < (size_t) modelRate / 2)
        return result;

    audio.insert (audio.begin(), (size_t) overlapLen / 2, 0.0f);
    std::vector<float> window ((size_t) windowLen), notes;

    for (size_t pos = 0; pos < audio.size(); pos += (size_t) hopLen)
    {
        const size_t have = std::min ((size_t) windowLen, audio.size() - pos);
        std::copy (audio.begin() + (long) pos, audio.begin() + (long) (pos + have), window.begin());
        std::fill (window.begin() + (long) have, window.end(), 0.0f);

        double energy = 0.0;
        for (size_t i = 0; i < have; ++i)
            energy += (double) window[i] * window[i];
        if (std::sqrt (energy / (double) have) < 0.0015)   // silence: the network would only invent notes
            continue;

        if (! runWindow (n, window.data(), notes))
            return result;

        // the frames at both ends of a window overlap their neighbours: the middle ones are used
        for (int f = overlapFrames / 2; f < framesPerWindow - overlapFrames / 2; ++f)
            for (int m = 0; m < numPitches; ++m)
            {
                const float p = notes[(size_t) f * numPitches + (size_t) m] - noteThreshold;
                if (p > 0.0f)
                    result.v[(m + 21) % 12] += p;
            }
    }

    for (float x : result.v)
        result.mass += x;
    result.valid = result.mass > 1.0f;
   #else
    juce::ignoreUnused (mono, sampleRate);
   #endif
    return result;
}

KeyGuess guessKey (const float* hist)
{
    KeyGuess g;
    double x[12];
    double sum = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        x[i] = hist[i];
        sum += x[i];
    }
    if (sum <= 1e-6)
        return g;

    double best = -2.0, second = -2.0;
    for (int t = 0; t < 12; ++t)
        for (int mode = 0; mode < 2; ++mode)
        {
            const double r = correlate (x, mode == 0 ? majorProfile : minorProfile, t);
            if (r > best)
            {
                second = best;
                best = r;
                g.tonic = t;
                g.minor = mode == 1;
            }
            else if (r > second)
                second = r;
        }
    g.corr = best;
    g.margin = best - second;
    return g;
}

juce::String keyName (int tonic, bool minor)
{
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (tonic < 0 || tonic > 11)
        return "-";
    return juce::String (names[tonic]) + (minor ? "m" : "");
}

juce::String keyNameFull (int tonic, bool minor)
{
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (tonic < 0 || tonic > 11)
        return "-";
    return juce::String (names[tonic]) + (minor ? " минор"_ru : " мажор"_ru);
}

juce::String keyEngineError()
{
    return lastError;
}
