#pragma once

#include "Engine.h"
#include "LyricsModel.h"
#include "Style.h"
#include "Text.h"
#include "TrackComponent.h"

// Lyrics of one song: the text and the sections (block of lines + the time it is sung).
// Stored next to the song file:
//   <song>.lyrics.txt  the text        <song>.lyrics.cue  one section per line: "first last start end" (lines, beats)
struct LyricsData
{
    juce::String text;
    LyricsSections sections;

    static juce::File textFile (const juce::File& songFile);
    static juce::File markFile (const juce::File& songFile);
    static juce::String decode (const juce::MemoryBlock& bytes);   // UTF-8 / UTF-16 / Windows-1251
    void load (const juce::File& songFile);
    void save (const juce::File& songFile) const;
    static void rename (const juce::File& from, const juce::File& to);
    static void remove (const juce::File& songFile);

    // text with [mm:ss.xx] in front of the lines (LRC): the plain text and one section per timed line
    static bool parseLrc (const juce::String& text, double beatSeconds, juce::String& plain, LyricsSections& sections);
};

// Small web server for the phone: the page scrolls by itself with the song and has the remote buttons.
//   /          the page          /lyrics  text and sections (JSON)          /state  position, section ...
//   /cmd?do=..&k=..  a button of the page (needs the key)
class LyricsServer : private juce::Thread
{
public:
    LyricsServer();
    ~LyricsServer() override;

    bool start();
    void setKey (const juce::String& k) { key = k; }
    std::function<void (const juce::String& command)> onCommand;   // called on the server thread
    int getPort() const { return port; }
    static juce::String findLocalAddress();      // the address of this PC in the local network

    // message thread
    void publishText (const juce::String& title, const juce::StringArray& lines, const std::vector<LyricsSection>& sections);
    void publishPlaylist (const juce::String& current, const juce::String& next, const juce::String& listJson);   // listJson: {name,index,items[],lists[]}
    void publishLive (double seconds, bool playing, bool paused, int section, int finished, double bpm, int semitones);

private:
    void run() override;
    void serve (juce::StreamingSocket& client);
    void reply (juce::StreamingSocket& client, const juce::String& type, const juce::String& body);

    juce::StreamingSocket listener;
    int port = 0;

    juce::CriticalSection lock;
    juce::String lyricsJson;
    int revision = 0;
    double livePos = 0.0;
    bool livePlaying = false, livePaused = false;
    int liveSection = -1, liveFinished = 0;
    double liveBpm = 0.0;
    int liveSemis = 0;
    juce::String key;
    juce::String curName, nextName, playlistJson;
    int plRevision = 0;
};

// The lyrics window: the text (editable, all the usual keys), a dividing line that you drag over the lines,
// "+" puts the lines above the dividing line on the timeline at the current moment.
class LyricsPanel : public juce::Component, public juce::FileDragAndDropTarget,
                    private juce::TextEditor::Listener, private juce::Timer
{
public:
    explicit LyricsPanel (Engine& e);
    ~LyricsPanel() override;

    std::function<void (double seconds)> onSeek;
    std::function<void (const juce::String& command)> onRemote;   // a button of the phone page (message thread)

    void setSong (const juce::String& name, const juce::File& songFile);   // the open song changed
    void setPlaylist (const juce::String& current, const juce::String& next, const juce::String& listJson);   // for the phone page

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    class Overlay;
    class TimelineView;

private:
    friend class Overlay;
    friend class TimelineView;

    static constexpr int gutterW = 58;

    void timerCallback() override;
    void textEditorTextChanged (juce::TextEditor&) override;

    // text and geometry
    void refreshLines();
    void updateGeometry();
    float lineY (int boundary) const { return geo.top0 + (float) boundary * geo.pitch; }
    int boundaryAtY (float y) const { return (int) std::round ((y - geo.top0) / geo.pitch); }
    float blockHeight (int first, int last) const;     // height of lines [first,last) on the phone screen
    bool fits (int first, int last) const;             // the block and the first row after it fit the phone screen
    int maxDivider (int firstLine) const;              // how far the dividing line can go from firstLine
    int pendingFirst() const;                  // first line of the block that "+" would take
    void setDivider (int boundary);            // the dividing line (dragged or from the counter)
    void resetDivider();

    // sections
    void addMarker();
    void deleteSelected();
    void sectionsChanged();                    // save, tell the phone, repaint
    void select (int index);
    void seekSeconds (double seconds);
    double beatSeconds() const { return engine.getBeatSeconds(); }
    double maxBeats() const;
    void showDeleteMenu (int index);
    void say (const juce::String& text);

    // files
    void chooseFile();
    void loadText (const juce::String& text);
    void saveNow();
    void publish();

    Engine& engine;
    LyricsData data;
    juce::File songFile;
    juce::String title;
    juce::StringArray lines;
    std::vector<int> lineStarts;
    juce::StringArray wrapped;        // every line cut into the rows of the phone screen (joined with \n)
    std::vector<int> rowCount;        // rows of every line on the phone screen (0: empty line or [tag])
    LyricsServer server;
    bool serverOk = false;
    juce::String url;

    struct Geo { float top0 = 4.0f, pitch = 24.0f; } geo, paintedGeo;
    int divider = 0, count = 8, selected = -1, playingSection = -1;
    bool dirty = false;
    juce::uint32 dirtyAt = 0, statusUntil = 0;
    double shownClock = -1.0;
    juce::String statusText;

    juce::TextButton loadButton, phoneButton, plusButton, deleteButton;
    juce::Label countLabel, infoLabel, timeLabel, hintLabel;
    juce::Slider countSlider;
    IconButton playButton { IconButton::Icon::Play }, pauseButton { IconButton::Icon::Pause }, stopButton { IconButton::Icon::Stop };
    juce::Component editorArea;
    juce::TextEditor editor;
    std::unique_ptr<Overlay> overlay;
    std::unique_ptr<TimelineView> timeline;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LyricsPanel)
};

class LyricsWindow : public juce::DocumentWindow
{
public:
    explicit LyricsWindow (Engine& e);   // starts hidden: the phone page works without the window
    void closeButtonPressed() override { setVisible (false); }
    LyricsPanel& getPanel() { return *panel; }

private:
    LyricsPanel* panel = nullptr;
};
