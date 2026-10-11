#pragma once

#include "Engine.h"
#include "TrackComponent.h"
#include "Timeline.h"
#include "Style.h"
#include "PlayerBar.h"
#include "Library.h"

class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& pluginToShow, std::function<void (PluginWindow*)> onCloseRequested);

    void closeButtonPressed() override { if (onClose) onClose (this); }
    juce::AudioPluginInstance* getPlugin() const { return plugin; }

private:
    juce::AudioPluginInstance* plugin;
    std::function<void (PluginWindow*)> onClose;
};

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // top bar / menus
    void showMainMenu();
    void showAudioSettings();
    void showPluginChooser();
    void showPluginScanner();
    void showAnalysisSettings();

    // strip menus
    void showInputMenu (const std::shared_ptr<Track>& track, juce::Component* anchor);
    void showSlotMenu (const std::shared_ptr<Track>& track, int slotIndex, juce::Component* anchor);
    void showOutputMenu (const std::shared_ptr<Track>& track, juce::Component* anchor);
    void loadPluginIntoSlot (const std::shared_ptr<Track>& track, int slotIndex, const juce::PluginDescription& desc);
    void openPluginEditor (const std::shared_ptr<Track>& track, int slotIndex);
    void closeEditorsFor (juce::AudioPluginInstance* plugin);
    void showClipMenu (const std::shared_ptr<Track>& track, const std::shared_ptr<Clip>& clip, juce::Point<int> screenPos);
    void showLaneMenu (const std::shared_ptr<Track>& track, double time, juce::Point<int> screenPos);
    void chooseAudioFor (const std::shared_ptr<Track>& track, double time);
    void addAudioFiles (const std::shared_ptr<Track>& track, const juce::StringArray& files, double time);
    void detectClipTempo (const std::shared_ptr<Clip>& clip);
    void zoomTimeline (float wheelDelta);
    void seekTimeline (double clockSeconds);
    void repaintTimeline();
    void layoutTimelineParts();
    // sequences (songs) and playlists
    void initialiseLibrary();
    void enterProject (const juce::String& preferredComposition);
    void switchProject (const juce::String& name);
    void createProject (const juce::String& wantedName);
    void renameProject();
    void deleteProject();
    void switchToSequence (const juce::String& name, int indexInPlaylist);
    void stepSong (int delta);
    void refreshPlayerBar();
    void preloadNeighbours();
    void showSequenceMenu();
    void showPlaylistMenu();
    void showAddTrackMenu();
    void showDeleteMenu();
    void createSequence (const juce::String& wantedName, bool addToPlaylist);
    void addToPlaylist (const juce::String& sequenceName);
    void renameSequence();
    void renamePlaylist();
    void savePlaylistNow();
    void saveEverything (bool announce);
    void showSongsMenu();
    void say (const juce::String& message);
    void askText (const juce::String& title, const juce::String& label, const juce::String& initial,
                  std::function<void (juce::String)> done);
    void confirmDelete (const juce::String& message, std::function<void()> done);
    juce::PopupMenu::Options optionsBelow (juce::Rectangle<int> localRectInPlayerBar);
    void addTrack();
    void tapTempo();
    void setPinned (bool pinned);

    void rebuildTracks();
    void refreshStrips();
    void layoutTracks();
    void timerCallback() override;
    void updateTimeline();
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    TcLookAndFeel lookAndFeel;   // must outlive every child component
    Engine engine;
    TimelineView timelineView;
    RulerComponent ruler { engine, timelineView };
    PlayheadOverlay playhead { engine, timelineView };
    juce::Label statusLabel;
    Library library { engine.getDataDirectory() };
    Playlist playlist;
    int playlistIndex = -1;
    PlayerBar playerBar;
    juce::TooltipWindow tooltipWindow { this, 600 };

    // top bar
    IconButton menuButton { IconButton::Icon::Menu };
    juce::Slider bpmSlider;
    juce::TextButton tapButton { "Tap" };
    juce::Label keyLabel { {}, "Тон: -"_ru };
    IconButton analysisButton { IconButton::Icon::Gear };
    juce::Label cpuLabel;
    IconButton playAllButton { IconButton::Icon::Play }, pauseAllButton { IconButton::Icon::Pause },
              stopAllButton { IconButton::Icon::Stop };

    // tracks
    juce::Viewport viewport;
    juce::Component trackList;
    juce::OwnedArray<TrackComponent> trackComponents;
    juce::TextButton addTrackButton { "+ Добавить дорожку"_ru };
    juce::Label emptyHint;

    juce::OwnedArray<PluginWindow> pluginWindows;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::PropertiesFile> pluginScanProps;
    juce::Array<juce::int64> tapTimes;
    bool pinned = true;
    int autosaveCounter = 0;
    int slowCounter = 0;
    double lastScroll = -1.0e18, lastSpan = 0.0, lastBpm = 0.0;
    double displayClock = 0.0, engineClockSeen = 0.0, engineClockStamp = 0.0, lastFrameStamp = 0.0;
    juce::VBlankAttachment vblank;
    bool timelineDirty = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
