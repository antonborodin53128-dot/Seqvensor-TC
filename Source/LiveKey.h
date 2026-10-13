#pragma once

#include "KeyDetect.h"
#include "TempoDetect.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <vector>

// Key detection from a live input channel (e.g. the channel of the mixer that carries the music), without
// any audio track: the audio thread copies the channel into a ring buffer, a background thread runs the
// network on the last half minute every few seconds.
class LiveKeyAnalyzer : private juce::Thread
{
public:
    LiveKeyAnalyzer();
    ~LiveKeyAnalyzer() override;

    struct State
    {
        bool active = false;       // an input channel is selected
        bool valid = false;        // a key has been found
        KeyGuess key;
        float level = 0.0f;        // RMS of the analysed audio
        double seconds = 0.0;      // how much audio the last result is based on
        double bpm = 0.0;          // tempo of the input (0 = not found yet)
        double bpmConfidence = 0.0;
        long long bpmStamp = 0;    // grows with every new tempo result
    };

    void prepare (double deviceRate);                    // message thread, when the device (re)starts
    void setChannels (int left, int right);              // input channel indexes, -1 = off; right = -1: mono
    void getChannels (int& left, int& right) const { left = chanL.load(); right = chanR.load(); }
    void push (const float* const* inputs, int numInputs, int numSamples);   // audio thread
    State getState() const;
    void setWanted (bool key, bool tempo) { wantKey.store (key); wantTempo.store (tempo); notify(); }   // what to analyse
    void reset();                                        // forget the old music (new song)

private:
    void run() override;

    std::atomic<int> chanL { -1 }, chanR { -1 };
    std::atomic<bool> wantKey { false }, wantTempo { false };
    std::atomic<double> rate { 48000.0 };
    std::vector<float> ring;
    std::atomic<long long> written { 0 };
    std::atomic<long long> resetAt { 0 };               // samples before this position are not analysed

    mutable juce::CriticalSection stateLock;
    State state;
};
