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
        const long long rs = resetAt.load();
        long long from = std::max (w - (long long) (sr * 30.0), rs);
        from = std::max (from, w - cap + (long long) sr);   // never read what the writer is about to overwrite
        const long long count = w - from;
        if ((double) count < sr * 6.0)
            continue;   // too little music for a verdict

        // a new song (reset, new channel, new device): the collected notes and the key on screen are dropped
        if (rs != seenReset || analysedTo > w)
        {
            seenReset = rs;
            analysedTo = rs;
            std::fill (std::begin (acc), std::end (acc), 0.0f);
            accSeconds = 0.0;
            shown = KeyGuess();
            pendTonic = -1;
            pendCount = 0;
        }
        analysedTo = std::max (analysedTo, from);   // what fell out of the ring before it was analysed is skipped

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

        // The key is judged from the whole song, not from the last half minute: only the audio that is new since the
        // last run goes through the network, and its notes are added to the song's histogram (the oldest part fades
        // with a half-life of three minutes, so a real key change still gets through).
        if (wantKey.load())
        {
            const long long newCount = w - analysedTo;
            if ((double) newCount >= sr * 3.0)
            {
                std::vector<float> fresh (chunk.end() - (long) newCount, chunk.end());
                const KeyHist h = computeKeyHist (fresh, sr);
                const double secs = (double) newCount / sr;
                analysedTo = w;
                if (h.valid)
                {
                    const float keep = (float) std::pow (0.5, secs / 180.0);
                    for (int i = 0; i < 12; ++i)
                        acc[i] = acc[i] * keep + h.v[i];
                    accSeconds += secs;
                }
            }

            if (accSeconds >= 8.0)
            {
                const KeyGuess cand = guessKey (acc);
                if (cand.tonic >= 0 && cand.corr >= 0.4)
                {
                    if (shown.tonic < 0)
                    {
                        shown = cand;
                    }
                    else if (cand.tonic == shown.tonic && cand.minor == shown.minor)
                    {
                        shown = cand;
                        pendCount = 0;
                    }
                    else
                    {
                        // another key: it must fit clearly better than the one on screen, twice in a row (in the first
                        // half minute a smaller lead is enough, so that an early wrong guess is corrected quickly)
                        const double gap = cand.corr - keyScore (acc, shown.tonic, shown.minor);
                        if (gap >= (accSeconds < 30.0 ? 0.02 : 0.04))
                        {
                            if (cand.tonic == pendTonic && cand.minor == pendMinor)
                                ++pendCount;
                            else
                            {
                                pendTonic = cand.tonic;
                                pendMinor = cand.minor;
                                pendCount = 1;
                            }
                            if (pendCount >= 2)
                            {
                                shown = cand;
                                pendCount = 0;
                                    }
                        }
                        else
                            pendCount = 0;
                    }
                }
            }
            s.key = shown;
            s.valid = shown.tonic >= 0;
            s.seconds = accSeconds;
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
