#pragma once

#include "Engine.h"
#include "Style.h"
#include "Text.h"

// Lyrics of one song: the lines and, per line, the moment it starts (in beats from the start of the song,
// so the marks stay in the right place when the tempo changes). Stored next to the song file:
//   <song>.lyrics.txt  the text,   <song>.lyrics.cue  one mark per line (empty = none)
struct LyricsData
{
    static constexpr double none = -1.0e9;

    juce::StringArray lines;
    std::vector<double> beats;

    bool isEmpty() const { return lines.isEmpty(); }
    void clear() { lines.clear(); beats.clear(); }

    // plain text, or text with [mm:ss.xx] marks in front of the lines (LRC): those become marks
    void setText (const juce::String& text, double beatSeconds);
    void clearMarks() { beats.assign (lines.size(), none); }
    bool hasMarks() const;
    int lineAt (double beat) const;                 // the line that is sung at this beat, -1 = none yet

    static juce::File textFile (const juce::File& songFile);
    static juce::File markFile (const juce::File& songFile);
    static juce::String decode (const juce::MemoryBlock& bytes);   // UTF-8 / UTF-16 / Windows-1251
    void load (const juce::File& songFile);
    void save (const juce::File& songFile) const;
    static void rename (const juce::File& from, const juce::File& to);
    static void remove (const juce::File& songFile);
};

// Small web server for the phone: the page scrolls by itself with the song.
//   /          the page          /lyrics  the lines and marks (JSON)          /state  position, playing, line
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
    void publishText (const juce::String& title, const juce::StringArray& lines);
    void publishLive (double seconds, bool playing, int line, double bpm, int semitones);

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
    bool livePlaying = false;
    int liveLine = -1;
    double liveBpm = 0.0;
    int liveSemis = 0;
    juce::String key;
};

// The window with the lyrics: big lines, the current one is lit; tap mode puts the marks while the song plays.
class LyricsPanel : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit LyricsPanel (Engine& e);
    ~LyricsPanel() override;

    std::function<void()> onTogglePlay;
    std::function<void (double seconds)> onSeek;
    std::function<void (const juce::String& command)> onRemote;   // a button of the phone page (message thread)

    void setSong (const juce::String& name, const juce::File& songFile);   // the open song changed
    void loadTextFile (const juce::File& f);

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    static constexpr int lineH = 44, headerH = 128;

    void timerCallback() override;
    void tap();
    void stepBack();
    void setTapMode (bool on);
    void changed();                       // text or marks changed: save and tell the phone
    void chooseFile();
    void pasteText();
    void publish();
    int nextTextLine (int from) const;
    int lineAtY (int y) const;
    double nowBeats() const { return engine.getClockSeconds() / engine.getBeatSeconds(); }
    juce::Rectangle<int> viewArea() const;

    Engine& engine;
    LyricsData data;
    juce::File songFile;
    juce::String title;
    LyricsServer server;
    bool serverOk = false;

    bool tapMode = false;
    int tapIndex = 0;
    int current = -1;
    float scrollY = 0.0f;
    juce::int64 manualUntil = 0;
    juce::String url;

    juce::TextButton loadButton, pasteButton, markButton, clearButton, copyButton;
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
