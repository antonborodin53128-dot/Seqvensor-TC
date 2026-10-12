#pragma once

#include "Clip.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

// What the engine's global clock (metronome + transport) does inside one audio chunk.
struct ChunkClock
{
    static constexpr int maxEvents = 16;
    bool running = false;
    double startSeconds = 0.0;      // clock time at the start of the chunk (0 = first beat, < 0 = count-in)
    int numEvents = 0;              // metronome beats inside the chunk
    int eventOffset[maxEvents] {};
    bool eventAccent[maxEvents] {};
};

// One mixer channel / timeline row: source (device input, another track = bus) + audio clips
// -> up to 5 plugin slots -> fader -> output pair.
// Every track has its own chain, so plugin latency of one track never delays another one
// unless "compensate" is switched on for that track.
class Track
{
public:
    static constexpr int numSlots = 5;

    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::PluginDescription description;
        std::atomic<bool> bypass { false };
    };

    explicit Track (juce::String trackName);
    ~Track();

    int id = 0;
    juce::String name;

    // Mixer / routing. Safe to change from the UI thread.
    std::atomic<int> inputL { -1 };      // index among the active device inputs, -1 = none
    std::atomic<int> inputR { -1 };      // -1 = mono (L is used for both sides)
    std::atomic<int> busId { -1 };       // >= 0: take the audio of the track with this id instead of an input
    std::atomic<int> outputPair { 0 };   // 0 = outputs 1-2, 1 = outputs 3-4 ..., -1 = no output
    std::atomic<float> gainLinear { 1.0f };
    std::atomic<bool> muted { false };
    std::atomic<bool> recArmed { false };
    std::atomic<bool> compensate { false };
    std::atomic<bool> metronomeOn { false };
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    std::atomic<int> loadingClips { 0 };
    bool inputChosenByUser = false;

    // Chain editing: message thread only.
    void setPlugin (int index, std::unique_ptr<juce::AudioPluginInstance> plugin, const juce::PluginDescription& description);
    void clearSlot (int index);
    void moveSlot (int from, int to);                                  // inside this chain, the others shift
    void swapSlotWith (int index, Track& other, int otherIndex);       // between two chains (or inside one)
    Slot* getSlot (int index) const;
    int findFreeSlot() const;
    int getNumPlugins() const;
    int computeLatencySamples() const;
    int getCachedLatencySamples() const { return cachedLatency.load(); }
    void refreshLatency() { cachedLatency.store (computeLatencySamples()); }

    // Timeline clips: message thread only (the audio thread reads them under clipLock).
    std::shared_ptr<Clip> addClip (std::shared_ptr<ClipData> data, double startSeconds, int clipId);
    void removeClip (int clipId);
    void replaceClipData (const std::shared_ptr<Clip>& clip, std::shared_ptr<ClipData> newData);   // swaps the audio of a clip
    const std::vector<std::shared_ptr<Clip>>& getClips() const { return clips; }
    double getEndSeconds() const;          // end of the last non-looping clip
    double getEarliestStart() const;       // start of the first clip (0 if none)

    // Audio thread.
    void prepare (double sampleRate, int maxBlockSize, juce::AudioPlayHead* playHead);
    void release();
    void setTempoSource (const std::atomic<double>* bpm) { bpmSource = bpm; }
    void setExtraDelay (int samples) { extraDelay.store (juce::jmax (0, samples)); }
    void processChunk (const float* const* inputs, int numInputs,
                       float* const* outputs, int numOutputs, int offset, int n,
                       const Track* busSource, const ChunkClock& clock);
    const juce::AudioBuffer<float>& getBusOut() const { return busOut; }

    // Used by the engine to keep the audio thread out of chain edits.
    juce::CriticalSection chainLock;

private:
    void renderClips (int n, const ChunkClock& clock);
    void renderClicks (int n, const ChunkClock& clock);
    void renderClick (float* l, float* r, int n);
    void runChain (int n);
    void applyExtraDelay (int n);

    std::array<std::unique_ptr<Slot>, numSlots> slots;

    std::vector<std::shared_ptr<Clip>> clips;
    juce::SpinLock clipLock;

    int clickPos = -1;
    bool clickAccent = false;
    const std::atomic<double>* bpmSource = nullptr;

    // audio state
    double sampleRate = 48000.0;
    int maxBlock = 512;
    juce::AudioBuffer<float> stereoBuf;
    juce::AudioBuffer<float> busOut;
    juce::AudioBuffer<float> clickBuf;
    juce::AudioBuffer<float> pluginBuf;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> delayBuf;
    int delayWritePos = 0;
    std::atomic<int> extraDelay { 0 };
    std::atomic<int> cachedLatency { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Track)
};
