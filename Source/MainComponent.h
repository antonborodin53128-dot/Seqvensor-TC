#pragma once

#include "Engine.h"
#include "TrackComponent.h"
#include "TempoControl.h"
#include "Timeline.h"
#include "Style.h"
#include "PlayerBar.h"
#include "Library.h"
#include "Lyrics.h"

class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& pluginToShow, std::function<void (PluginWindow*)> onCloseRequested,
                  std::function<bool()> isBypassed = {}, std::function<void()> toggleBypass = {});

    void closeButtonPressed() override { if (onClose) onClose (this); }
    juce::AudioPluginInstance* getPlugin() const { return plugin; }

private:
    juce::AudioPluginInstance* plugin;
    std::function<void (PluginWindow*)> onClose;
};

class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    // top bar / menus
    void showMainMenu();
    void exportScene (juce::String songName = {});
    void chooseRecordFolder();
    void toggleRecording();
    void importSceneFile (const juce::File& file);
    void importSequenceFile (const juce::File& file);
    void importPlaylistFile (const juce::File& file);
    void importFromFile();
    void exportPlaylist (juce::String name = {});
    void saveSelectionToFile (PlayerBar::Tab tab);
    void exportSequence (juce::String project = {});
    void showAudioSettings();
    void showPluginChooser();
    void showPluginScanner();
    void showAnalysisSettings();
    void showKeyMenu();
    void updateKeyDisplay();
    void updateAutoTempo();
    long long mixSignature = 0, liveBpmStamp = 0;
    double liveBpmCandidate = 0.0;
    int liveBpmAgree = 0;
    int keyTextCounter = 0;

    // strip menus
    void showInputMenu (const std::shared_ptr<Track>& track, juce::Component* anchor);
    void showSlotMenu (const std::shared_ptr<Track>& track, int slotIndex, juce::Component* anchor);
    void showOutputMenu (const std::shared_ptr<Track>& track, juce::Component* anchor);
    void loadPluginIntoSlot (const std::shared_ptr<Track>& track, int slotIndex, const juce::PluginDescription& desc);
    void openPluginEditor (const std::shared_ptr<Track>& track, int slotIndex);
    void slotDropped (const std::shared_ptr<Track>& target, int targetIndex, const juce::String& payload, bool copy);
    void closeEditorsFor (juce::AudioPluginInstance* plugin);
    void showClipMenu (const std::shared_ptr<Track>& track, const std::shared_ptr<Clip>& clip, juce::Point<int> screenPos);
    void showLaneMenu (const std::shared_ptr<Track>& track, double time, juce::Point<int> screenPos);
    void chooseAudioFor (const std::shared_ptr<Track>& track, double time);
    void addAudioFiles (const std::shared_ptr<Track>& track, const juce::StringArray& files, double time);
    std::shared_ptr<Track> trackOf (const std::shared_ptr<Clip>& clip) const;
    void zoomTimeline (float wheelDelta);
    void seekTimeline (double clockSeconds);
    void repaintTimeline();
    void layoutTimelineParts();
    // sequences (songs) and playlists
    void initialiseLibrary();
    void enterProject (juce::String preferredComposition);
    void switchProject (juce::String name);
    void createProject (const juce::String& wantedName);
    void renameProject();
    void deleteSelectedRows (PlayerBar::Tab tab);
    void deleteProject();
    void switchToSequence (juce::String name, int indexInPlaylist);
    void stepSong (int delta);
    void refreshPlayerBar();
    void createLyricsWindow();
    void showQr();                     // QR tab of the player bar
    void publishGuitars();             // guitarists and scenes for the phone page
    void showLyrics();                 // the lyrics window (T button)
    void syncLyrics();                 // shows the lyrics of the open song
    void preloadNeighbours();
    void showAddTrackMenu();
    void showDeleteMenu();
    void onPlayerRowClicked (PlayerBar::Tab tab, int id);
    void loadPlaylistByName (juce::String name);
    void createPlaylist (const juce::String& wantedName);
    void deletePlaylistConfirm();
    void createSequence (const juce::String& wantedName, bool addToPlaylist);
    void addToPlaylist (const juce::String& sequenceName);
    void renameSequence();
    void renamePlaylist();
    void savePlaylistNow();
    void saveEverything (bool announce);
    void say (const juce::String& message);
    void askText (const juce::String& title, const juce::String& label, const juce::String& initial,
                  std::function<void (juce::String)> done);
    void confirmDelete (const juce::String& message, std::function<void()> done);
    // clip selection
    std::vector<std::shared_ptr<Clip>> selectedClips() const;
    void clipPressed (const std::shared_ptr<Track>& track, const std::shared_ptr<Clip>& clip, const juce::ModifierKeys& mods);
    void dragSelectedClips (const std::shared_ptr<Clip>& primary, double deltaSeconds);
    void emptyPressed (const juce::ModifierKeys& mods);
    void marqueeSelect (juce::Rectangle<int> screenRect, bool finished);
    void selectAllClips();
    void deleteSelectedClips();
    void addTrack();
    void tapTempo();
    void keepOnTop();
    void raiseMenus();
    void setPinned (bool pinned);

    void rebuildTracks();
    int trackRowForDrag (const juce::String& description, juce::Point<int> pos, int& from) const;
    void trackDragMove (const juce::String& description, juce::Point<int> pos);
    void trackDropped (const juce::String& description, juce::Point<int> pos);
    void onPlayerRowMoved (PlayerBar::Tab tab, int from, int to);
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
    static constexpr int scrollerH = 30;
    TimelineScroller scroller { engine, timelineView };
    TimeReadout timeReadout;
    juce::Label statusLabel;
    Library library { engine.getDataDirectory() };
    Playlist playlist;
    int playlistIndex = -1;
    juce::String phoneListSignature, phoneListJson;
    std::vector<juce::File> recentSongs;   // last visited songs, kept loaded
    int anchorRow = -1;
    std::set<int> marqueeBase;
    std::vector<std::pair<std::shared_ptr<Clip>, double>> dragOrigins;
    juce::StringArray projectRows, playlistRows, otherTracks;
    PlayerBar playerBar;
    juce::TooltipWindow tooltipWindow { this, 600 };

    // top bar
    IconButton menuButton { IconButton::Icon::Menu };
    TempoControl bpmSlider;
    juce::TextButton tapButton { "Tap" };
    bool liveKeyReset = false;                                 // the live key was dropped after silence (until the next signal)
    juce::TextButton keyLabel { "key" };                       // click: menu with the key of the mix
    juce::TextButton keyDown { "-" }, keyUp { "+" };           // transposition of the whole mix, semitones
    IconButton analysisButton { IconButton::Icon::Gear };
    juce::Label cpuLabel;
    IconButton playAllButton { IconButton::Icon::Play }, pauseAllButton { IconButton::Icon::Pause },
              stopAllButton { IconButton::Icon::Stop };
    IconButton recordButton { IconButton::Icon::Record };
    juce::Label recTimeLabel;

    // tracks
    juce::Viewport viewport;
    TrackListArea trackList;
    struct DropLine : public juce::Component
    {
        DropLine() { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override { g.setColour (Pal::accent); g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (8.0f, 0.0f), 2.0f); }
    } trackDropLine;
    juce::OwnedArray<TrackComponent> trackComponents;
    juce::TextButton addTrackButton { "+ Добавить дорожку"_ru };
    juce::Label emptyHint;

    juce::OwnedArray<PluginWindow> pluginWindows;
    std::unique_ptr<LyricsWindow> lyricsWindow;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::PropertiesFile> pluginScanProps;
    juce::Array<juce::int64> tapTimes;
    bool pinned = false;
    int autosaveCounter = 0;
    int slowCounter = 0;
    double lastScroll = -1.0e18, lastSpan = 0.0, lastBpm = 0.0;
    double displayClock = 0.0, engineClockSeen = 0.0, engineClockStamp = 0.0, lastFrameStamp = 0.0;
    juce::VBlankAttachment vblank;
    bool timelineDirty = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
