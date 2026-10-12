#pragma once

#include "Track.h"
#include "Text.h"

#include <functional>
#include <mutex>
#include <map>
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

// One song: its tracks, clips, plugins and tempo. Sequences of the neighbouring songs are kept loaded
// (and prepared) in memory, so switching between songs is instant.
struct Sequence
{
    juce::String name;
    juce::File file;
    double bpm = 120.0;
    std::vector<std::shared_ptr<Track>> tracks;   // while the sequence is not the active one
    std::atomic<int> pending { 0 };               // clips / plugins that are still loading
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

    // Global transport. One clock (seconds, 0 = bar 1 beat 1, < 0 = count-in) drives the
    // metronome and the audio clips of every track.
    void playAll();                                     // start / resume
    void pauseAll();                                    // freezes the clock, Play continues from here
    void stopAll();                                     // stops everything and rewinds
    void seek (double clockSeconds);                    // move the song position
    bool isPlaying() const;
    bool isPaused() const { return paused.load(); }
    double getClockSeconds() const { return clockNow.load(); }
    double getSongEnd() const { return songEnd.load(); }   // end of the last clip, seconds
    juce::String describeState() const;
    void logLine (const juce::String& m) const;
    bool isAudioAlive() const;                          // audio device open and delivering blocks
    juce::int64 getCallbackCount() const { return callbackCount.load(); }
    juce::String audioInitError;
    double getBeatSeconds() const { return 60.0 / getBpm(); }

    // Audio clips (message thread). The file is decoded on a background thread.
    void addClipFromFile (const std::shared_ptr<Track>& track, const juce::File& file, double startSeconds,
                          std::function<void (std::shared_ptr<Clip> clip, const juce::String& error)> done = {});
    void removeClip (const std::shared_ptr<Track>& track, int clipId);
    void clipsChanged();                                // recomputes the end of the song
    // Time-stretch the audio of a clip to a project tempo (pitch unchanged), in the background. The newest request wins.
    void stretchClip (const std::shared_ptr<Track>& track, const std::shared_ptr<Clip>& clip, double targetBpm,
                      std::function<void()> done = {});
    int takeClipId() { return nextClipId++; }

    double getSampleRate() const { return currentSampleRate; }
    int getBlockSize() const { return currentBlockSize; }
    float getCpuLoad() const { return (float) loadMeasure.getLoadAsProportion(); }
    juce::String describeDeviceLatency() const;

    void setBpm (double newBpm) { playHead.bpm.store (juce::jlimit (20.0, 400.0, newBpm)); }
    double getBpm() const { return playHead.bpm.load(); }

    // Persistence.
    juce::File getDataDirectory() const;
    void savePluginList() const;
    juce::File getLegacyProjectFile() const { return getDataDirectory().getChildFile ("project.xml"); }

    // Sequences (songs). Message thread only.
    std::shared_ptr<Sequence> obtainSequence (const juce::File& file);   // cached, or loaded from disk (empty if no file yet)
    std::shared_ptr<Sequence> getActiveSequence() const { return activeSequence; }
    void activateSequence (const std::shared_ptr<Sequence>& seq);         // stops the transport, swaps the tracks
    void saveActiveSequence();
    void dropSequences (const std::vector<juce::File>& keep);              // frees cached sequences (the active one stays)
    void renameSequenceFile (const juce::File& from, const juce::File& to);
    void forgetSequence (const juce::File& file);
    void relocateSequences (const juce::File& oldDir, const juce::File& newDir);   // a folder was renamed

    std::function<void()> onTracksChanged;

    // AudioIODeviceCallback
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;

private:
    std::shared_ptr<Track> makeTrack (const juce::String& name, int id);
    void fillSequence (const std::shared_ptr<Sequence>& seq);
    void sequenceProgress (const std::shared_ptr<Sequence>& seq);
    void updateCompensation();
    void loadEnabledPlugins();
    void saveEnabledPlugins() const;

    std::set<juce::String> enabledPlugins;
    bool allPluginsEnabled = true;
    int nextTrackId = 1;
    int nextClipId = 1;
    std::atomic<double> songEnd { 0.0 };

    std::atomic<bool> clockRunning { false };
    std::atomic<bool> paused { false };
    std::atomic<double> clockNow { 0.0 };       // clock time at the end of the last audio block
    std::mutex stretchMutex;                    // one time-stretch job at a time
    std::atomic<int> command { 0 };             // 1 = (re)start the clock at commandClock
    std::atomic<double> commandClock { 0.0 };
    double clockSeconds = 0.0;                  // audio thread only
    int beatIndex = 0;
    double nextBeatIn = 0.0;
    std::vector<Track*> order;

    std::map<juce::String, std::shared_ptr<Sequence>> sequenceCache;
    std::shared_ptr<Sequence> activeSequence;

    mutable juce::CriticalSection tracksLock;
    std::vector<std::shared_ptr<Track>> tracks;
    juce::AudioProcessLoadMeasurer loadMeasure;

    double currentSampleRate = 48000.0;
    int currentBlockSize = 512;
    bool running = false;
    std::atomic<juce::int64> callbackCount { 0 };
    std::atomic<juce::int64> lockFailures { 0 };
    std::atomic<int> stopReason { 0 };
    std::atomic<double> stopClock { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};
