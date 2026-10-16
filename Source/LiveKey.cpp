#include "LiveKey.h"

#include <algorithm>
#include <cmath>

LiveKeyAnalyzer::LiveKeyAnalyzer() : juce::Thread ("TC live key")
{
    ring.assign ((size_t) (48000.0 * 40.0), 0.0f);
    startThread (juce::Thread::Priority::low);
}

LiveKeyAnalyzer::~LiveKeyAnalyzer()
{
    stopThread (4000);
}

void LiveKeyAnalyzer::prepare (double deviceRate)
{
    if (deviceRate <= 0.0)
        return;
    // the thread only reads the ring while the rate is the one it was sized for
    if (std::abs (deviceRate - rate.load()) > 0.5 || ring.empty())
    {
        const int l = chanL.exchange (-1);   // pause the writer while the buffer is replaced
        std::vector<float> fresh ((size_t) (deviceRate * 40.0), 0.0f);
        {
            const juce::ScopedLock sl (stateLock);
            ring.swap (fresh);
            rate.store (deviceRate);
            written.store (0);
            lastSignal.store (-1);
            resetAt.store (0);
        }
        chanL.store (l);
    }
}

void LiveKeyAnalyzer::setChannels (int left, int right)
{
    chanR.store (left >= 0 ? right : -1);
    chanL.store (left);
    lastSignal.store (-1);
    reset();
    {
        const juce::ScopedLock sl (stateLock);
        state = State();
        state.active = left >= 0;
    }
    notify();
}

void LiveKeyAnalyzer::reset()
{
    resetAt.store (written.load());
}

void LiveKeyAnalyzer::push (const float* const* inputs, int numInputs, int numSamples)
{
    const int l = chanL.load();
    if (l < 0 || l >= numInputs || inputs[l] == nullptr || ring.empty())
        return;
    const int r = chanR.load();
    const float* a = inputs[l];
    const float* b = (r >= 0 && r < numInputs && inputs[r] != nullptr) ? inputs[r] : nullptr;

    const long long cap = (long long) ring.size();
    long long w = written.load (std::memory_order_relaxed);
    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float v = b != nullptr ? 0.5f * (a[i] + b[i]) : a[i];
        ring[(size_t) ((w + i) % cap)] = v;
        peak = std::max (peak, std::abs (v));
    }
    written.store (w + numSamples, std::memory_order_release);
    if (peak > 0.002f)   // about -54 dB: above the noise floor of an open input
        lastSignal.store (w + numSamples, std::memory_order_relaxed);
}

double LiveKeyAnalyzer::silenceSeconds() const
{
    const long long last = lastSignal.load (std::memory_order_relaxed);
    if (last < 0)
        return 1.0e9;
    return (double) (written.load (std::memory_order_relaxed) - last) / juce::jmax (1.0, rate.load());
}

LiveKeyAnalyzer::State LiveKeyAnalyzer::getState() const
{
    const juce::ScopedLock sl (stateLock);
    State s = state;
    s.active = chanL.load() >= 0;
    return s;
}

void LiveKeyAnalyzer::run()
{
    std::vector<float> chunk;
    while (! threadShouldExit())
    {
        wait (4000);
        if (threadShouldExit())
            break;
        if (chanL.load() < 0 || (! wantKey.load() && ! wantTempo.load()))
            continue;

        const double sr = rate.load();
        const long long cap = (long long) ring.size();
        const long long w = written.load (std::memory_order_acquire);
        long long from = std::max (w - (long long) (sr * 30.0), resetAt.load());
        from = std::max (from, w - cap + (long long) sr);   // never read what the writer is about to overwrite
        const long long count = w - from;
        if ((double) count < sr * 6.0)
            continue;   // too little music for a verdict

        chunk.resize ((size_t) count);
        {
            const juce::ScopedLock sl (stateLock);   // the ring is not replaced while it is copied
            if ((long long) ring.size() != cap)
                continue;
            for (long long i = 0; i < count; ++i)
                chunk[(size_t) i] = ring[(size_t) ((from + i) % cap)];
        }

        double e = 0.0;
        for (float x : chunk)
            e += (double) x * x;
        const float rms = (float) std::sqrt (e / (double) count);

        State s;
        s.active = true;
        s.level = rms;
        s.seconds = (double) count / sr;

        if (wantKey.load())
        {
            const KeyHist h = computeKeyHist (chunk, sr);
            if (h.valid)
            {
                s.key = guessKey (h.v);
                s.valid = s.key.tonic >= 0 && s.key.corr >= 0.4;
            }
        }

        TempoResult tempo;
        const bool tempoRun = wantTempo.load() && rms >= 0.0015f;
        if (tempoRun)
        {
            const size_t keep = std::min ((size_t) count, (size_t) (sr * 20.0));
            std::vector<float> tail (chunk.end() - (long) keep, chunk.end());
            tempo = detectTempoEx (tail, sr);
        }

        const juce::ScopedLock sl (stateLock);
        if (wantKey.load() && (s.valid || ! state.valid || rms < 0.0015f))
        {
            const double bpm = state.bpm, conf = state.bpmConfidence;
            const long long stamp = state.bpmStamp;
            state = s;           // silence or noise keeps showing the last good key
            state.bpm = bpm;
            state.bpmConfidence = conf;
            state.bpmStamp = stamp;
        }
        else
            state.level = rms;

        if (tempoRun && tempo.confidence >= tempoMinConfidence)
        {
            state.bpm = tempo.bpm;
            state.bpmConfidence = tempo.confidence;
            ++state.bpmStamp;
        }
    }
}
