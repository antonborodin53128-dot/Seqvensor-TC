#pragma once

#include <juce_core/juce_core.h>

#include <vector>

// Musical key detection. A neural network (Spotify Basic Pitch, Apache 2.0, run through ONNX Runtime) turns the
// audio into note probabilities; the notes are collected in a 12-bin pitch-class histogram, and the key is the
// major / minor profile (Krumhansl-Schmuckler) that fits the histogram best. Histograms of several tracks add up,
// so a whole song can be judged from its separate instruments and vocals.
struct KeyHist
{
    float v[12] = {};        // sum of the note probabilities per pitch class (C = 0)
    float mass = 0.0f;       // sum of v
    bool valid = false;      // false: the network was not available or the audio was silent
};

struct KeyGuess
{
    int tonic = -1;          // 0 = C ... 11 = B, -1 = unknown
    bool minor = false;
    double corr = 0.0;       // correlation with the best profile: how tonal the material is (0..1)
    double margin = 0.0;     // distance to the second best key
};

// Blocking (about a second per minute of audio): call from a background thread.
KeyHist computeKeyHist (const std::vector<float>& mono, double sampleRate);

// The histogram does not have to be normalised.
KeyGuess guessKey (const float* hist);

juce::String keyName (int tonic, bool minor);            // "Am", "C", "F#m"
juce::String keyNameFull (int tonic, bool minor);        // "A минор", "C мажор"

// For the log / the tooltip: empty when the network works.
juce::String keyEngineError();
