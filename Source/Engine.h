#pragma once

#include "Track.h"
#include "Text.h"

#include <functional>
#include <set>

// Tempo / transport information that every hosted plugin can read (delays, arpeggiators ...).
class TcPlayHead : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        const double tempo = bpm.load();
        const double rate = sampleRate.load();
        const juce::int64 pos = samplePos.load();
        const double seconds = rate > 0.0 ? (double) pos / rate : 0.0;

        info.setBpm (tempo);
        info.setTimeSignature (TimeSignature { 4, 4 });
        info.setIsPlaying (playing.load());
        info.setIsRecording (false);
        info.setIsLooping (false);
        info.setTimeInSamples (pos);
        info.setTimeInSeconds (seconds);
        info.setPpqPosition (seconds * tempo / 60.0);
        return info;
    }

    void advance (int numSamples) { samplePos.store (samplePos.load() + numSamples); }

    std::atomic<double> bpm { 120.0 };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<bool> playing { false };
    std::atomic<juce::int64> samplePos { 0 };
};

class Engine : public juce::AudioIODeviceCallback
{
public:
    Engine();
    ~Engine() override;

    // Call once after construction (loads saved audio settings, starts the device).
    void initialise();
    void shutdown();

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager pluginFormats;
    juce::AudioFormatManager audioFormats;
    juce::KnownPluginList knownPlugins;
    TcPlayHead playHead;

    // Tracks (message thread).
    std::shared_ptr<Track> addTrack (const juce::String& name, int forcedId = -1);
    void removeTrack (const std::shared_ptr<Track>& track);
    std::vector<std::shared_ptr<Track>> getTracks() const;
    std::shared_ptr<Track> findTrack (int id) const;
    // true if taking the audio of track `sourceId` into `track` would create a loop
    bool wouldCreateCycle (const Track& track, int sourceId) const;

    // Which plugins are offered in the track slot menus (ticked in the plugin list dialog).
    bool isPluginEnabled (const juce::PluginDescription& d) const;
    void setPluginEnabled (const juce::PluginDescription& d, bool enabled);
    void setAllPluginsEnabled (bool enabled);
    std::vector<juce::PluginDescription> getEnabledEffects() const;

    // Plugins (message thread). The callback runs on the message thread.
    void addPluginToTrack (const std::shared_ptr<Track>& track, int slotIndex, const juce::PluginDescription& description,
                           std::function<void (bool ok, const juce::String& error)> done,
                           const juce::String& savedStateBase64 = {}, bool bypassed = false);

    // Global transport. One clock (seconds, 0 = first beat, < 0 = count-in) drives the metronome
    // and every player. The metronome is independent of the players: it keeps clicking when a
    // player is stopped. A player that is started joins at the current clock time.
    void playAll();                                     // start / resume (all players)
    void playTrack (const std::shared_ptr<Track>& t);   // this player only (joins a running clock)
    void pauseAll();                                    // freezes the clock, Play continues from here
    void stopAll();                                     // stops everything and rewinds
    void seek (double clockSeconds);                    // move the song position
    bool isPlaying() const;
    bool isPaused() const { return paused.load(); }
    bool isGridMode() const { return gridMode.load(); }  // true: audio offsets and the count-in are in use
    double getClockSeconds() const { return clockNow.load(); }

    double getSampleRate() const { return currentSampleRate; }
    int getBlockSize() const { return currentBlockSize; }
    float getCpuLoad() const { return (float) loadMeasure.getLoadAsProportion(); }
    juce::String describeDeviceLatency() const;

    void setBpm (double newBpm) { playHead.bpm.store (juce::jlimit (20.0, 400.0, newBpm)); }
    double getBpm() const { return playHead.bpm.load(); }

    // Persistence.
    juce::File getDataDirectory() const;
    void savePluginList() const;
    void saveProject();
    void loadProject();
    juce::File getProjectFile() const { return getDataDirectory().getChildFile ("project.xml"); }

    std::function<void()> onTracksChanged;

    // AudioIODeviceCallback
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;

private:
    void updateCompensation();
    void loadEnabledPlugins();
    void saveEnabledPlugins() const;

    std::set<juce::String> enabledPlugins;
    bool allPluginsEnabled = true;
    int nextTrackId = 1;
    void startTransport (const std::vector<std::shared_ptr<Track>>& toPlay);
    void armTrack (Track& t, double clockTime, bool grid);

    std::atomic<bool> clockRunning { false };
    std::atomic<bool> paused { false };
    std::atomic<bool> gridMode { false };
    std::atomic<double> clockNow { 0.0 };       // clock time at the end of the last audio block
    std::atomic<int> command { 0 };             // 1 = (re)start the clock at commandClock
    std::atomic<double> commandClock { 0.0 };
    double clockSeconds = 0.0;                  // audio thread only
    int beatIndex = 0;
    double nextBeatIn = 0.0;
    std::vector<Track*> order;

    mutable juce::CriticalSection tracksLock;
    std::vector<std::shared_ptr<Track>> tracks;
    juce::TimeSliceThread readThread { "TC file reader" };
    juce::AudioProcessLoadMeasurer loadMeasure;

    double currentSampleRate = 48000.0;
    int currentBlockSize = 512;
    bool running = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};
