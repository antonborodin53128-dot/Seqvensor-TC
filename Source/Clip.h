#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <memory>
#include <vector>

// A whole audio file decoded into memory (random access => instant seeks, sample-accurate
// placement on the timeline) plus a min/max overview for drawing.
struct ClipData
{
    static constexpr int peakBlock = 128;

    juce::String name;
    juce::File file;
    double sampleRate = 44100.0;
    int numSamples = 0;
    juce::AudioBuffer<float> audio;      // 1 or 2 channels
    std::vector<float> peaks;            // min, max per block of peakBlock samples

    double lengthSeconds() const { return sampleRate > 0.0 ? (double) numSamples / sampleRate : 0.0; }

    // Decodes the file (call from a background thread). Returns null and sets `error` on failure.
    static std::shared_ptr<ClipData> load (const juce::File& file, juce::String& error);

    // Draws the part [t0, t1) (seconds inside the clip) into the rectangle.
    void draw (juce::Graphics& g, juce::Rectangle<int> area, double t0, double t1) const;

    // Mono mix of a region, for analysis (tempo detection).
    void getMono (std::vector<float>& out, int maxSamples) const;
};

// A clip placed on a track.
struct Clip
{
    int id = 0;
    std::shared_ptr<ClipData> data;
    std::atomic<double> start { 0.0 };   // seconds on the timeline (0 = bar 1, beat 1)
    std::atomic<bool> loop { false };    // repeat until the song ends

    double end() const { return start.load() + (data != nullptr ? data->lengthSeconds() : 0.0); }
};
