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
        int requestedChannels = 2;   // channel layout asked from the plugin (1 = mono, 2 = stereo); message thread only
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
    std::atomic<int> outputChannel { -1 };   // mono track: one output channel (0 = output 1); -1 = use outputPair (or none)
    std::atomic<bool> mono { false };        // one channel through the chain; set it with setMono()
    std::atomic<float> gainLinear { 1.0f };
    std::atomic<bool> muted { false };
    std::atomic<bool> soloed { false };
    std::atomic<bool> othersSoloed { false };   // set by the engine every chunk: some other track is in solo
    std::atomic<float> pan { 0.0f };            // -1 = left ... +1 = right
    std::atomic<bool> recArmed { false };
    std::atomic<bool> compensate { false };
    std::atomic<bool> metronomeOn { false };
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    std::atomic<float> rmsL { 0.0f }, rmsR { 0.0f };   // loudest block RMS since the display last looked
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

    // Mono / stereo (message thread). The audio fades out for a moment, the plugins get the new channel layout,
    // the audio fades back in: no clicks. A plugin that cannot do mono keeps stereo and gets the mono signal on both channels.
    void setMono (bool on);
    void conformLayouts();     // plugin layouts follow the mono flag (after a plugin was added or moved here)

    // Timeline clips: message thread only (the audio thread reads them under clipLock).
    std::shared_ptr<Clip> addClip (std::shared_ptr<ClipData> data, double startSeconds, int clipId,
                                   std::shared_ptr<LiveStretch> live = nullptr);
    void setLive (const std::shared_ptr<Clip>& clip, std::shared_ptr<LiveStretch> live);
    void removeClip (int clipId);
    // swaps the audio of a clip; if it is sounding right now, it carries on at the same musical point
    void replaceClipData (const std::shared_ptr<Clip>& clip, std::shared_ptr<ClipData> newData, double clockNow);
    // tempo changed by factor r = oldBpm / newBpm: clips stay on their bars, sounding audio stays continuous
    void retempo (double r, double clockNow, double newBpm);
    void clearShifts();
    const std::vector<std::shared_ptr<Clip>>& getClips() const { return clips; }
    double getEndSeconds() const;          // end of the last non-looping clip
    double getEarliestStart() const;       // start of the first clip (0 if none)

    // Audio thread.
    void prepare (double sampleRate, int maxBlockSize, juce::AudioPlayHead* playHead);
    void release();
    void setTempoSource (const std::atomic<double>* bpm) { bpmSource = bpm; }
    void setPitchSource (const std::atomic<int>* semitones) { pitchSource = semitones; }   // project transposition
    std::atomic<bool> transposeOn { true };    // off for drums etc.: the project transposition does not touch the track
    bool isTransposed() const { return transposeOn.load() && pitchSource != nullptr && pitchSource->load() != 0; }
    void setExtraDelay (int samples) { extraDelay.store (juce::jmax (0, samples)); }
    void processChunk (const float* const* inputs, int numInputs,
                       float* const* outputs, int numOutputs, int offset, int n,
                       const Track* busSource, const ChunkClock& clock);
    const juce::AudioBuffer<float>& getBusOut() const { return busOut; }

    // Recording of the raw input of this track into a wav file. Message thread; the audio thread only feeds the writer.
    bool startRecording (const juce::File& file, double sampleRate, juce::TimeSliceThread& thread);
    void stopRecording();
    bool isRecording() const { return recActive.load(); }

    // Used by the engine to keep the audio thread out of chain edits.
    juce::CriticalSection chainLock;

private:
    void renderClips (int n, const ChunkClock& clock);
    void renderClicks (int n, const ChunkClock& clock);
    void renderClick (float* l, float* r, int n);
    void runChain (int n, bool isMono);
    void relayout (bool toMono);
    void applyExtraDelay (int n);

    std::array<std::unique_ptr<Slot>, numSlots> slots;

    std::vector<std::shared_ptr<Clip>> clips;
    juce::SpinLock clipLock;

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recWriter;
    juce::CriticalSection recLock;
    std::atomic<bool> recActive { false };
    int clickPos = -1;
    bool clickAccent = false;
    const std::atomic<double>* bpmSource = nullptr;
    const std::atomic<int>* pitchSource = nullptr;

    // audio state
    double sampleRate = 48000.0;
    int maxBlock = 512;
    juce::AudioBuffer<float> stereoBuf;
    juce::AudioBuffer<float> busOut;
    juce::AudioBuffer<float> clipScratch;
    juce::AudioBuffer<float> clickBuf;
    juce::AudioBuffer<float> pluginBuf;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> delayBuf;
    int delayWritePos = 0;
    float lastPanL = 1.0f, lastPanR = 1.0f;
    std::atomic<int> extraDelay { 0 };
    std::atomic<int> cachedLatency { 0 };
    std::atomic<bool> switching { false };            // mono <-> stereo change in progress: the audio is faded out
    std::atomic<juce::int64> lastChunkMs { 0 };       // when the audio thread last processed this track
    float fadeGain = 1.0f;
    int recChannels = 2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Track)
};
