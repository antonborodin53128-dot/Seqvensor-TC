#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <memory>
#include <vector>

// One independent signal path: live input or file player -> plugin chain -> output pair.
// Every track has its own chain, so plugin latency of one track never delays another one
// unless "compensate" is switched on for that track.
class Track
{
public:
    enum class Kind { Live, Player };

    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::PluginDescription description;
        std::atomic<bool> bypass { false };
    };

    Track (Kind kindToUse, juce::String trackName,
           juce::AudioFormatManager& formats, juce::TimeSliceThread& readThread);
    ~Track();

    const Kind kind;
    juce::String name;

    // Mixer / routing. Safe to change from the UI thread.
    std::atomic<int> inputL { 0 };      // index among the active device inputs, -1 = none
    std::atomic<int> inputR { -1 };     // -1 = mono (L is used for both sides)
    std::atomic<int> outputPair { 0 };  // 0 = outputs 1-2, 1 = outputs 3-4, ...
    std::atomic<float> gainLinear { 1.0f };
    std::atomic<bool> muted { false };
    std::atomic<bool> compensate { false };
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };

    // Chain editing: message thread only.
    void addPlugin (std::unique_ptr<juce::AudioPluginInstance> plugin, const juce::PluginDescription& description);
    void removePlugin (int index);
    int getNumPlugins() const;
    Slot* getSlot (int index) const;
    int computeLatencySamples() const;                       // message thread
    int getCachedLatencySamples() const { return cachedLatency.load(); }
    void refreshLatency() { cachedLatency.store (computeLatencySamples()); }

    // Player: message thread only.
    bool loadFile (const juce::File& file);
    juce::File getFile() const { return currentFile; }
    void play();
    void pause();
    void stopAndRewind();
    bool isPlaying() const { return transport.isPlaying(); }
    double getLengthSeconds() const { return transport.getLengthInSeconds(); }
    double getPositionSeconds() const { return transport.getCurrentPosition(); }
    void setPositionSeconds (double seconds) { transport.setPosition (seconds); }
    void setLooping (bool shouldLoop);
    bool isLooping() const { return looping; }

    // Audio thread.
    void prepare (double sampleRate, int maxBlockSize, juce::AudioPlayHead* playHead);
    void release();
    void setExtraDelay (int samples) { extraDelay.store (juce::jmax (0, samples)); }
    void process (const float* const* inputs, int numInputs,
                  float* const* outputs, int numOutputs, int numSamples);

    // Used by the engine to keep the audio thread out of chain edits.
    juce::CriticalSection chainLock;

private:
    void processChunk (const float* const* inputs, int numInputs,
                       float* const* outputs, int numOutputs, int offset, int n);
    void runChain (int n);
    void applyExtraDelay (int n);

    juce::AudioFormatManager& formatManager;
    juce::TimeSliceThread& readThread;

    std::vector<std::unique_ptr<Slot>> slots;

    // player
    juce::File currentFile;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioTransportSource transport;
    int fileChannels = 0;
    bool looping = false;

    // audio state
    double sampleRate = 48000.0;
    int maxBlock = 512;
    juce::AudioBuffer<float> stereoBuf;
    juce::AudioBuffer<float> pluginBuf;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> delayBuf;
    int delayWritePos = 0;
    std::atomic<int> extraDelay { 0 };
    std::atomic<int> cachedLatency { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Track)
};
