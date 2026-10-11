#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

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

    // Built-in player (+ metronome): message thread only.
    bool loadFile (const juce::File& file);
    juce::File getFile() const { return currentFile; }
    // Audio shift against the metronome grid (seconds, > 0 = later, < 0 = earlier).
    void setClipOffset (double seconds);
    double getClipOffset() const { return clipOffset.load(); }
    void play();                           // this player only, no count-in
    void prepareStart (int countInBeats);  // seeks the file (before a start request)
    void requestStart (int countInBeats) { startRequest.store (countInBeats); }
    void haltTransport();                  // stops without rewinding (safe from any thread)
    void stopAndRewind();
    bool isPlaying() const { return running.load(); }
    bool hasAudio() const { return hasFile.load(); }
    double getLengthSeconds() const { return transport.getLengthInSeconds(); }
    double getPositionSeconds() const { return transport.getCurrentPosition(); }
    void setPositionSeconds (double seconds) { transport.setPosition (seconds); }
    void setLooping (bool shouldLoop);
    bool isLooping() const { return looping; }

    // Audio thread.
    void prepare (double sampleRate, int maxBlockSize, juce::AudioPlayHead* playHead);
    void release();
    void setTempoSource (const std::atomic<double>* bpm) { bpmSource = bpm; }
    void setExtraDelay (int samples) { extraDelay.store (juce::jmax (0, samples)); }
    void processChunk (const float* const* inputs, int numInputs,
                       float* const* outputs, int numOutputs, int offset, int n,
                       const Track* busSource);
    const juce::AudioBuffer<float>& getBusOut() const { return busOut; }

    // Used by the engine to keep the audio thread out of chain edits.
    juce::CriticalSection chainLock;

private:
    void renderTransport (int n);
    void renderClick (float* l, float* r, int n);
    double beatSeconds() const { return 60.0 / juce::jlimit (20.0, 400.0, bpmSource != nullptr ? bpmSource->load() : 120.0); }
    // Position in the file where playback has to start: if the audio is shifted earlier than
    // the count-in allows, the part that does not fit is skipped.
    double startPositionFor (int countInBeats) const
    {
        if (countInBeats <= 0)
            return 0.0;   // no metronome: the song simply plays from its very beginning
        return juce::jmax (0.0, -((double) countInBeats * beatSeconds() + clipOffset.load()));
    }
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
    std::atomic<bool> running { false };
    std::atomic<int> startRequest { -1 };   // >= 0: start with that many count-in beats
    std::atomic<double> clipOffset { 0.0 };
    int countIn = 0;
    int fileDelayLeft = 0;
    int beatIndex = 0;
    double nextBeatIn = 0.0;
    bool musicStarted = false;
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
