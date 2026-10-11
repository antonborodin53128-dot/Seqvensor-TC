#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
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

// One mixer channel: source (device input, another track = bus, or the built-in player)
// -> up to 5 slots (VST3 plugins / built-in player) -> gain -> output pair.
// Every track has its own chain, so plugin latency of one track never delays another one
// unless "compensate" is switched on for that track.
class Track
{
public:
    static constexpr int numSlots = 5;

    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;   // null for the built-in player
        juce::PluginDescription description;
        std::atomic<bool> bypass { false };
        bool builtinPlayer = false;
    };

    Track (juce::String trackName, juce::AudioFormatManager& formats, juce::TimeSliceThread& readThread);
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

    // Chain editing: message thread only.
    void setPlugin (int index, std::unique_ptr<juce::AudioPluginInstance> plugin, const juce::PluginDescription& description);
    void setPlayerSlot (int index);
    void clearSlot (int index);
    Slot* getSlot (int index) const;
    int findPlayerSlot() const;
    int findFreeSlot() const;
    int getNumPlugins() const;
    int computeLatencySamples() const;
    int getCachedLatencySamples() const { return cachedLatency.load(); }
    void refreshLatency();                 // also refreshes the "player active" flag
    bool isPlayerActive() const { return playerActive.load(); }

    // Built-in player: message thread only. The engine's global clock is the time base:
    // the file is placed on the clock and started at a given clock time (see Engine).
    bool loadFile (const juce::File& file);
    juce::File getFile() const { return currentFile; }
    // Seeks the file to `filePositionSeconds` and starts pulling it when the clock reaches `startClockSeconds`.
    void armPlayback (double startClockSeconds, double filePositionSeconds);
    void stopPlayer();                     // silences the player only; the metronome is independent
    bool isPlaying() const { return playerState.load() != 0; }
    bool hasEndedNaturally() const { return endedNaturally.load(); }
    void clearEnded() { endedNaturally.store (false); }
    bool hasAudio() const { return hasFile.load(); }
    double getLengthSeconds() const { return transport.getLengthInSeconds(); }
    double getPositionSeconds() const { return transport.getCurrentPosition(); }
    void setLooping (bool shouldLoop);
    bool isLooping() const { return looping; }

    // Audio shift against the metronome grid (seconds, > 0 = later, < 0 = earlier, at most 4 beats).
    void setClipOffset (double seconds);
    double getClipOffset() const { return clipOffset.load(); }
    double beatSeconds() const { return 60.0 / juce::jlimit (20.0, 400.0, bpmSource != nullptr ? bpmSource->load() : 120.0); }

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
    void renderTransport (int n, const ChunkClock& clock);
    void renderClick (float* l, float* r, int n);
    void runChain (int n);
    void applyExtraDelay (int n);

    juce::AudioFormatManager& formatManager;
    juce::TimeSliceThread& readThread;

    std::array<std::unique_ptr<Slot>, numSlots> slots;

    // player
    juce::File currentFile;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioTransportSource transport;
    int fileChannels = 0;
    bool looping = false;
    std::atomic<bool> hasFile { false };
    std::atomic<bool> playerActive { false };
    std::atomic<int> playerState { 0 };            // 0 stopped, 1 armed (waiting for the clock), 2 playing
    std::atomic<double> pullStartClock { 0.0 };
    std::atomic<bool> endedNaturally { false };
    std::atomic<double> clipOffset { 0.0 };
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
