#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

// A whole audio file decoded into memory (random access => instant seeks, sample-accurate
// placement on the timeline) plus a min/max overview for drawing.
struct ClipData
{
    static constexpr int peakBlock = 128;

    juce::String name;
    juce::File file;
    double fileBpm = 0.0;                // tempo of the original file (0 = not detected)
    double tempoConfidence = 0.0;        // 0..1, how sure the detector was
    float keyHist[12] = {};              // notes of the file per pitch class (key detection), mass 0 = unknown
    float keyMass = 0.0f;
    double sampleRate = 44100.0;
    int numSamples = 0;
    juce::AudioBuffer<float> audio;      // 1 or 2 channels
    std::vector<float> peaks;            // min, max per block of peakBlock samples
    std::atomic<int> decoded { 0x7fffffff };   // samples already filled in (a file still being decoded plays silence beyond it)

    bool isComplete() const { return decoded.load() >= numSamples; }

    double lengthSeconds() const { return sampleRate > 0.0 ? (double) numSamples / sampleRate : 0.0; }

    void buildPeaks();                   // min / max overview for drawing

    // Decodes the file (call from a background thread). Returns null and sets `error` on failure.
    static std::shared_ptr<ClipData> load (const juce::File& file, juce::String& error);

    // Two steps, so that a clip can be shown at once: open() is quick (length, empty zeroed buffer),
    // fill() then decodes the audio chunk by chunk (background thread) and keeps `decoded` / the overview up to date.
    static std::shared_ptr<ClipData> open (const juce::File& file, juce::String& error,
                                           std::unique_ptr<juce::AudioFormatReader>& readerOut);
    void fill (juce::AudioFormatReader& reader, const std::function<void()>& progress);

    // Draws the part [t0, t1) (seconds inside the clip) into the rectangle.
    void draw (juce::Graphics& g, juce::Rectangle<int> area, double t0, double t1) const;

    // Mono mix of a region, for analysis (tempo detection).
    void getMono (std::vector<float>& out, int maxSamples) const;
};

class LiveStretch;

// A clip placed on a track.
struct Clip
{
    int id = 0;
    std::shared_ptr<ClipData> data;       // what is played: the file itself or its time-stretched copy
    std::shared_ptr<ClipData> original;   // the file as decoded (never changed)
    std::atomic<double> start { 0.0 };   // seconds on the timeline (0 = bar 1, beat 1)
    std::atomic<bool> loop { false };    // repeat until the song ends
    std::atomic<double> shift { 0.0 };   // playing only: keeps the audio continuous when the tempo changes (seconds)

    // tempo handling (the pitch always stays the same)
    std::atomic<bool> followTempo { false };      // re-stretch whenever the project tempo changes
    std::atomic<double> manualBpm { 0.0 };        // tempo of the file set by the user (0 = none): used when it could not be detected
    std::atomic<double> stretchedToBpm { 0.0 };   // tempo the played audio was stretched to (0 = untouched)
    std::atomic<double> stretchTarget { 0.0 };    // tempo of the stretch in progress (0 = none)
    std::atomic<int> stretchGen { 0 };            // newest request wins

    // Live following: while followTempo is on, the original is played through a real-time stretcher at the
    // project tempo. `speed` = project tempo / file tempo; the clip lasts file length / speed.
    std::shared_ptr<LiveStretch> live;            // replaced only under the track's clipLock
    std::atomic<bool> liveBuilding { false };
    std::atomic<double> speed { 1.0 };

    bool follows() const { return followTempo.load() && original != nullptr && original->fileBpm > 0.0; }

    void updateSpeed (double projectBpm)
    {
        double s = 1.0;
        if (follows() && projectBpm > 0.0)
        {
            s = projectBpm / original->fileBpm;
            if (s < 0.25 || s > 4.0)
                s = 1.0;
        }
        speed.store (s);
    }

    double effectiveBpm() const
    {
        const double s = stretchedToBpm.load();
        return s > 0.0 ? s : (original != nullptr ? original->fileBpm : 0.0);
    }

    double lengthSeconds() const
    {
        const ClipData* d = follows() ? original.get() : data.get();
        return d != nullptr ? d->lengthSeconds() / speed.load() : 0.0;
    }

    // trimming (non-destructive): fractions of the whole clip cut off at the beginning / the end. The audio does not move:
    // `start` is where the whole file would begin, the visible part begins at start + trimIn * length.
    std::atomic<double> trimIn { 0.0 }, trimOut { 0.0 };
    bool isTrimmed() const { return ! loop.load() && (trimIn.load() > 0.0 || trimOut.load() > 0.0); }
    double visibleStart() const { return loop.load() ? start.load() : start.load() + trimIn.load() * lengthSeconds(); }
    double end() const { return start.load() + (loop.load() ? 1.0 : 1.0 - trimOut.load()) * lengthSeconds(); }
};
