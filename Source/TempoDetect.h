#pragma once

#include <vector>

struct TempoResult
{
    double bpm = 0.0;          // 0 = nothing found
    double confidence = 0.0;   // 0..1
};

constexpr double tempoMinConfidence = 0.35;

// Tempo of a mono signal with a confidence value.
TempoResult detectTempoEx (const std::vector<float>& mono, double sampleRate);

// Tempo in BPM, or 0 when the result is not trustworthy.
double detectTempo (const std::vector<float>& mono, double sampleRate);
