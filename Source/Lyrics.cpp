#include "Lyrics.h"

#include <cstring>

// ---------------------------------------------------------------- data

namespace
{
    // "01:23.45" -> seconds
    bool parseTimeTag (const juce::String& inner, double& seconds)
    {
        const auto minutes = inner.upToFirstOccurrenceOf (":", false, false);
        const auto secs = inner.fromFirstOccurrenceOf (":", false, false);
        if (! inner.contains (":") || minutes.isEmpty() || secs.isEmpty()
            || ! minutes.containsOnly ("0123456789") || ! secs.containsOnly ("0123456789."))
            return false;
        seconds = minutes.getIntValue() * 60.0 + secs.getDoubleValue();
        return true;
    }

    bool isLrcInfoTag (const juce::String& line)
    {
        if (! line.startsWithChar ('[') || ! line.endsWithChar (']'))
            return false;
        const auto key = line.substring (1).upToFirstOccurrenceOf (":", false, false).toLowerCase();
        return line.contains (":") && (key == "ar" || key == "ti" || key == "al" || key == "by" || key == "offset"
                                       || key == "length" || key == "re" || key == "ve" || key == "au");
    }

    juce::juce_wchar windows1251 (unsigned char c)
    {
        if (c < 0x80)
            return (juce::juce_wchar) c;
        if (c >= 0xC0)
            return (juce::juce_wchar) (0x410 + (c - 0xC0));
        switch (c)
        {
            case 0xA8: return 0x401;   // Yo
            case 0xB8: return 0x451;   // yo
            case 0x85: return 0x2026;
            case 0x91: return 0x2018;
            case 0x92: return 0x2019;
            case 0x93: return 0x201C;
            case 0x94: return 0x201D;
            case 0x96: return 0x2013;
            case 0x97: return 0x2014;
            case 0xAB: return 0xAB;
            case 0xBB: return 0xBB;
            case 0xB9: return 0x2116;
            case 0xA0: return ' ';
            default:   return '?';
        }
    }
}

juce::String LyricsData::decode (const juce::MemoryBlock& bytes)
{
    const auto* p = static_cast<const unsigned char*> (bytes.getData());
    const size_t n = bytes.getSize();
    if (n == 0)
        return {};

    if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF)))
    {
        const bool little = p[0] == 0xFF;
        juce::String s;
        for (size_t i = 2; i + 1 < n; i += 2)
        {
            const unsigned c = little ? (unsigned) (p[i] | (p[i + 1] << 8)) : (unsigned) ((p[i] << 8) | p[i + 1]);
            s += juce::String::charToString ((juce::juce_wchar) c);
        }
        return s;
    }

    size_t offset = 0;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        offset = 3;

    const auto* text = reinterpret_cast<const char*> (p) + offset;
    const int length = (int) (n - offset);
    if (juce::CharPointer_UTF8::isValidString (text, length))
        return juce::String::fromUTF8 (text, length);

    juce::String s;
    for (size_t i = offset; i < n; ++i)
        s += juce::String::charToString (windows1251 (p[i]));
    return s;
}

void LyricsData::setText (const juce::String& text, double beatSeconds)
{
    lines.clear();
    beats.clear();

    auto all = juce::StringArray::fromLines (text);
    for (auto& raw : all)
    {
        auto line = raw.trimEnd();
        if (isLrcInfoTag (line.trim()))
            continue;

        double mark = none;
        while (line.trimStart().startsWithChar ('['))
        {
            const auto t = line.trimStart();
            const int close = t.indexOfChar (']');
            double seconds = 0.0;
            if (close < 0 || ! parseTimeTag (t.substring (1, close), seconds))
                break;
            if (mark == none)
                mark = seconds / juce::jmax (0.001, beatSeconds);
            line = t.substring (close + 1).trimStart();
        }
        lines.add (line);
        beats.push_back (mark);
    }

    while (! lines.isEmpty() && lines[lines.size() - 1].trim().isEmpty())
    {
        lines.remove (lines.size() - 1);
        beats.pop_back();
    }
    while (! lines.isEmpty() && lines[0].trim().isEmpty())
    {
        lines.remove (0);
        beats.erase (beats.begin());
    }
}

bool LyricsData::hasMarks() const
{
    for (auto b : beats)
        if (b > none / 2)
            return true;
    return false;
}

int LyricsData::lineAt (double beat) const
{
    int best = -1;
    double bestMark = none;
    for (size_t i = 0; i < beats.size(); ++i)
        if (beats[i] > none / 2 && beats[i] <= beat && beats[i] >= bestMark)
        {
            best = (int) i;
            bestMark = beats[i];
        }
    return best;
}

juce::File LyricsData::textFile (const juce::File& songFile)
{
    return songFile.getParentDirectory().getChildFile (songFile.getFileNameWithoutExtension() + ".lyrics.txt");
}

juce::File LyricsData::markFile (const juce::File& songFile)
{
    return songFile.getParentDirectory().getChildFile (songFile.getFileNameWithoutExtension() + ".lyrics.cue");
}

void LyricsData::load (const juce::File& songFile)
{
    clear();
    if (songFile == juce::File())
        return;

    const auto tf = textFile (songFile);
    if (! tf.existsAsFile())
        return;

    juce::MemoryBlock block;
    tf.loadFileAsData (block);
    lines = juce::StringArray::fromLines (decode (block));
    beats.assign ((size_t) lines.size(), none);

    const auto mf = markFile (songFile);
    if (mf.existsAsFile())
    {
        const auto marks = juce::StringArray::fromLines (mf.loadFileAsString());
        for (int i = 0; i < marks.size() && i < lines.size(); ++i)
            if (marks[i].trim().isNotEmpty())
                beats[(size_t) i] = marks[i].getDoubleValue();
    }
}

void LyricsData::save (const juce::File& songFile) const
{
    if (songFile == juce::File())
        return;

    const auto tf = textFile (songFile), mf = markFile (songFile);
    if (lines.isEmpty())
    {
        tf.deleteFile();
        mf.deleteFile();
        return;
    }

    tf.replaceWithText (lines.joinIntoString ("\n"), false, false, "\n");
    if (! hasMarks())
    {
        mf.deleteFile();
        return;
    }

    juce::StringArray marks;
    for (size_t i = 0; i < beats.size(); ++i)
        marks.add (beats[i] > none / 2 ? juce::String (beats[i], 4) : juce::String());
    mf.replaceWithText (marks.joinIntoString ("\n"), false, false, "\n");
}

void LyricsData::rename (const juce::File& from, const juce::File& to)
{
    if (textFile (from).existsAsFile())
        textFile (from).moveFileTo (textFile (to));
    if (markFile (from).existsAsFile())
        markFile (from).moveFileTo (markFile (to));
}

void LyricsData::remove (const juce::File& songFile)
{
    textFile (songFile).deleteFile();
    markFile (songFile).deleteFile();
}

// ---------------------------------------------------------------- phone page

static const char* phonePage = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>TC</title>
<style>
html,body{margin:0;background:#000;color:#f2f2f7;font-family:-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-text-size-adjust:100%}
#t{position:fixed;top:0;left:0;right:0;padding:10px 16px;background:rgba(17,17,17,.92);color:#98989f;font-size:15px;z-index:2;display:flex;justify-content:space-between}
#w{padding:45vh 18px 55vh}
.l{font-size:7.2vw;line-height:1.28;color:#d6d6db;padding:.18em 0;min-height:.6em;transition:color .25s}
.l.p{color:#5a5a60}
.l.c{color:#ffa81f;font-weight:700}
@media(min-width:700px){.l{font-size:44px}}
</style></head><body>
<div id="t"><span id="n">TC</span><span id="s"></span></div>
<div id="w"></div>
<script>
var rev=-1,els=[],cur=-2,manual=0,msg="";
function get(u){return fetch(u,{cache:"no-store"}).then(function(r){return r.json()})}
function build(d){
  var w=document.getElementById("w");w.innerHTML="";els=[];cur=-2;
  document.getElementById("n").textContent=d.title||"TC";
  if(!d.lines.length){var e=document.createElement("div");e.className="l p";e.textContent=d.empty;w.appendChild(e);return}
  d.lines.forEach(function(x){var e=document.createElement("div");e.className="l";e.textContent=x;w.appendChild(e);els.push(e)});
}
function show(c){
  if(c===cur)return;cur=c;
  els.forEach(function(e,i){e.className="l"+(c>=0&&i<c?" p":"")+(i===c?" c":"")});
  if(c>=0&&els[c]&&Date.now()>manual)els[c].scrollIntoView({block:"center",behavior:"smooth"});
}
function poll(){
  get("/state").then(function(s){
    document.getElementById("s").textContent=s.playing?"▶":"❚❚";
    if(s.rev!==rev){rev=s.rev;return get("/lyrics").then(build).then(function(){show(s.cur)})}
    show(s.cur);
  }).catch(function(){document.getElementById("s").textContent="✕"});
}
["touchstart","wheel","mousedown"].forEach(function(n){window.addEventListener(n,function(){manual=Date.now()+4000},{passive:true})});
document.addEventListener("click",function(){try{navigator.wakeLock.request("screen")}catch(e){}},{once:true});
setInterval(poll,250);poll();
</script></body></html>)HTML";

// ---------------------------------------------------------------- server

LyricsServer::LyricsServer() : juce::Thread ("TC lyrics server") {}

LyricsServer::~LyricsServer()
{
    signalThreadShouldExit();
    listener.close();
    stopThread (2000);
}

bool LyricsServer::start()
{
    for (int p = 8765; p < 8785; ++p)
        if (listener.createListener (p))
        {
            port = p;
            startThread();
            return true;
        }
    return false;
}

juce::String LyricsServer::findLocalAddress()
{
    juce::String fallback;
    for (auto& a : juce::IPAddress::getAllAddresses())
    {
        const auto s = a.toString();
        if (s == "127.0.0.1" || s.startsWith ("169.254.") || s.contains (":"))
            continue;
        if (s.startsWith ("192.168.") || s.startsWith ("10.") || s.startsWith ("172."))
            return s;
        if (fallback.isEmpty())
            fallback = s;
    }
    return fallback.isEmpty() ? juce::String ("127.0.0.1") : fallback;
}

void LyricsServer::publishText (const juce::String& title, const juce::StringArray& lines)
{
    auto* obj = new juce::DynamicObject();
    juce::Array<juce::var> arr;
    for (auto& l : lines)
        arr.add (l);
    obj->setProperty ("title", title);
    obj->setProperty ("lines", arr);
    obj->setProperty ("empty", "Текст не загружен"_ru);

    const juce::ScopedLock sl (lock);
    lyricsJson = juce::JSON::toString (juce::var (obj), true);
    ++revision;
}

void LyricsServer::publishLive (double seconds, bool playing, int line)
{
    const juce::ScopedLock sl (lock);
    livePos = seconds;
    livePlaying = playing;
    liveLine = line;
}

void LyricsServer::run()
{
    while (! threadShouldExit())
    {
        std::unique_ptr<juce::StreamingSocket> client (listener.waitForNextConnection());
        if (client != nullptr)
            serve (*client);
        else if (! threadShouldExit())
            wait (50);
    }
}

void LyricsServer::reply (juce::StreamingSocket& client, const juce::String& type, const juce::String& body)
{
    const int size = (int) body.getNumBytesAsUTF8();
    const auto head = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "; charset=utf-8\r\nContent-Length: " + juce::String (size)
                    + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";

    auto writeAll = [&client] (const char* data, int length)
    {
        int done = 0;
        while (done < length)
        {
            if (client.waitUntilReady (false, 1000) != 1)
                return;
            const int n = client.write (data + done, length - done);
            if (n <= 0)
                return;
            done += n;
        }
    };
    writeAll (head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
    writeAll (body.toRawUTF8(), size);
}

void LyricsServer::serve (juce::StreamingSocket& client)
{
    char buffer[2048];
    int got = 0;
    for (int tries = 0; tries < 10 && got < (int) sizeof (buffer) - 1; ++tries)
    {
        if (client.waitUntilReady (true, 300) != 1)
            break;
        const int n = client.read (buffer + got, (int) sizeof (buffer) - 1 - got, false);
        if (n <= 0)
            break;
        got += n;
        buffer[got] = 0;
        if (std::strstr (buffer, "\r\n\r\n") != nullptr)
            break;
    }
    if (got <= 0)
        return;

    const auto request = juce::String::fromUTF8 (buffer, got);
    const auto firstLine = request.upToFirstOccurrenceOf ("\r\n", false, false);
    const auto path = firstLine.fromFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf (" ", false, false)
                               .upToFirstOccurrenceOf ("?", false, false);

    if (path == "/state")
    {
        juce::String json;
        {
            const juce::ScopedLock sl (lock);
            json = "{\"rev\":" + juce::String (revision) + ",\"pos\":" + juce::String (livePos, 2)
                 + ",\"playing\":" + (livePlaying ? "true" : "false") + ",\"cur\":" + juce::String (liveLine) + "}";
        }
        reply (client, "application/json", json);
    }
    else if (path == "/lyrics")
    {
        juce::String json;
        {
            const juce::ScopedLock sl (lock);
            json = lyricsJson.isEmpty() ? juce::String ("{\"title\":\"TC\",\"lines\":[],\"empty\":\"\"}") : lyricsJson;
        }
        reply (client, "application/json", json);
    }
    else if (path == "/" || path == "/index.html")
        reply (client, "text/html", juce::String::fromUTF8 (phonePage));
    else
    {
        const juce::String head = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        client.write (head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
    }
}

// ---------------------------------------------------------------- panel

LyricsPanel::LyricsPanel (Engine& e) : engine (e)
{
    loadButton.setButtonText ("Загрузить..."_ru);
    pasteButton.setButtonText ("Вставить"_ru);
    markButton.setButtonText ("Разметка"_ru);
    clearButton.setButtonText ("Сбросить метки"_ru);
    copyButton.setButtonText ("Адрес для телефона"_ru);

    loadButton.setTooltip ("Загрузить текст из файла (.txt, .lrc)"_ru);
    pasteButton.setTooltip ("Вставить текст из буфера обмена"_ru);
    markButton.setTooltip ("Режим разметки: пока играет песня, жмите Enter в начале каждой строки"_ru);
    clearButton.setTooltip ("Убрать все метки времени, текст останется"_ru);
    copyButton.setTooltip ("Скопировать адрес страницы для телефона (телефон и ПК в одной сети)"_ru);

    markButton.setClickingTogglesState (true);
    loadButton.onClick = [this] { chooseFile(); };
    pasteButton.onClick = [this] { pasteText(); };
    markButton.onClick = [this] { setTapMode (markButton.getToggleState()); };
    clearButton.onClick = [this] { data.clearMarks(); tapIndex = juce::jmax (0, nextTextLine (0)); changed(); };
    copyButton.onClick = [this]
    {
        if (url.isEmpty())
            return;
        juce::SystemClipboard::copyTextToClipboard (url);
        copyButton.setButtonText ("Скопировано"_ru);
        juce::Component::SafePointer<LyricsPanel> safe (this);
        juce::Timer::callAfterDelay (1500, [safe] { if (safe != nullptr) safe->copyButton.setButtonText ("Адрес для телефона"_ru); });
    };

    for (auto* b : { &loadButton, &pasteButton, &markButton, &clearButton, &copyButton })
    {
        b->setWantsKeyboardFocus (false);
        b->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    serverOk = server.start();
    if (serverOk)
        url = "http://" + LyricsServer::findLocalAddress() + ":" + juce::String (server.getPort());

    setWantsKeyboardFocus (true);
    startTimerHz (30);
}

LyricsPanel::~LyricsPanel()
{
    stopTimer();
}

juce::Rectangle<int> LyricsPanel::viewArea() const
{
    return getLocalBounds().withTrimmedTop (headerH).withTrimmedBottom (28);
}

void LyricsPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 0);
    auto row1 = r.withTrimmedTop (36).removeFromTop (30);
    auto row2 = r.withTrimmedTop (72).removeFromTop (30);
    const int gap = 8;
    const int w1 = (row1.getWidth() - 2 * gap) / 3;
    loadButton.setBounds (row1.removeFromLeft (w1));
    row1.removeFromLeft (gap);
    pasteButton.setBounds (row1.removeFromLeft (w1));
    row1.removeFromLeft (gap);
    markButton.setBounds (row1);

    const int w2 = (row2.getWidth() - gap) / 2;
    clearButton.setBounds (row2.removeFromLeft (w2));
    row2.removeFromLeft (gap);
    copyButton.setBounds (row2);
}

int LyricsPanel::nextTextLine (int from) const
{
    for (int i = juce::jmax (0, from); i < data.lines.size(); ++i)
        if (data.lines[i].trim().isNotEmpty())
            return i;
    return -1;
}

void LyricsPanel::setSong (const juce::String& name, const juce::File& file)
{
    if (file == songFile && name == title)
        return;

    songFile = file;
    title = name;
    data.load (file);
    tapIndex = juce::jmax (0, nextTextLine (0));
    current = -1;
    scrollY = 0.0f;
    markButton.setToggleState (false, juce::dontSendNotification);
    tapMode = false;
    publish();
    repaint();
}

void LyricsPanel::publish()
{
    if (serverOk)
        server.publishText (title, data.lines);
}

void LyricsPanel::changed()
{
    data.save (songFile);
    publish();
    repaint();
}

void LyricsPanel::loadTextFile (const juce::File& f)
{
    juce::MemoryBlock block;
    if (! f.existsAsFile() || ! f.loadFileAsData (block))
        return;
    data.setText (LyricsData::decode (block), engine.getBeatSeconds());
    tapIndex = juce::jmax (0, nextTextLine (0));
    scrollY = 0.0f;
    changed();
}

void LyricsPanel::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Текст песни"_ru, juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   "*.txt;*.lrc");
    juce::Component::SafePointer<LyricsPanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe != nullptr && fc.getResult().existsAsFile())
                                  safe->loadTextFile (fc.getResult());
                          });
}

void LyricsPanel::pasteText()
{
    const auto text = juce::SystemClipboard::getTextFromClipboard();
    if (text.trim().isEmpty())
        return;
    data.setText (text, engine.getBeatSeconds());
    tapIndex = juce::jmax (0, nextTextLine (0));
    scrollY = 0.0f;
    changed();
}

void LyricsPanel::setTapMode (bool on)
{
    tapMode = on;
    markButton.setToggleState (on, juce::dontSendNotification);
    if (on)
    {
        const int cur = data.lineAt (nowBeats());
        tapIndex = juce::jmax (0, cur >= 0 ? nextTextLine (cur + 1) : nextTextLine (0));
        if (tapIndex < 0 || tapIndex >= data.lines.size())
            tapIndex = juce::jmax (0, nextTextLine (0));
        grabKeyboardFocus();
    }
    repaint();
}

void LyricsPanel::tap()
{
    if (! tapMode || data.lines.isEmpty() || tapIndex >= data.lines.size())
        return;
    const double clock = engine.getClockSeconds();
    if (clock < 0.0)
        return;   // still counting in

    data.beats[(size_t) tapIndex] = clock / engine.getBeatSeconds();
    const int next = nextTextLine (tapIndex + 1);
    tapIndex = next < 0 ? data.lines.size() : next;
    changed();
}

void LyricsPanel::stepBack()
{
    if (! tapMode || data.lines.isEmpty())
        return;
    int i = juce::jmin (tapIndex, data.lines.size()) - 1;
    while (i >= 0 && data.lines[i].trim().isEmpty())
        --i;
    if (i < 0)
        return;
    data.beats[(size_t) i] = LyricsData::none;
    tapIndex = i;
    changed();
}

bool LyricsPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey)
    {
        tap();
        return true;
    }
    if (key == juce::KeyPress::backspaceKey)
    {
        stepBack();
        return true;
    }
    if (key == juce::KeyPress::spaceKey)
    {
        if (onTogglePlay)
            onTogglePlay();
        return true;
    }
    if (key == juce::KeyPress::escapeKey && tapMode)
    {
        setTapMode (false);
        return true;
    }
    return false;
}

int LyricsPanel::lineAtY (int y) const
{
    const auto v = viewArea();
    const int i = (int) std::floor ((y - v.getY() - 20 + scrollY) / (float) lineH);
    return i >= 0 && i < data.lines.size() ? i : -1;
}

void LyricsPanel::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (tapMode && viewArea().contains (e.getPosition()))
    {
        const int i = lineAtY (e.y);
        if (i >= 0)
        {
            tapIndex = i;
            repaint();
        }
    }
}

void LyricsPanel::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! viewArea().contains (e.getPosition()))
        return;
    const int i = lineAtY (e.y);
    if (i >= 0 && data.beats[(size_t) i] > LyricsData::none / 2 && onSeek)
        onSeek (data.beats[(size_t) i] * engine.getBeatSeconds());
}

void LyricsPanel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const float maxY = juce::jmax (0.0f, (float) (data.lines.size() * lineH - viewArea().getHeight() / 2));
    scrollY = juce::jlimit (0.0f, maxY, scrollY - w.deltaY * 260.0f);
    manualUntil = juce::Time::currentTimeMillis() + 4000;
    repaint();
}

bool LyricsPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (f.endsWithIgnoreCase (".txt") || f.endsWithIgnoreCase (".lrc"))
            return true;
    return false;
}

void LyricsPanel::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files)
        if (f.endsWithIgnoreCase (".txt") || f.endsWithIgnoreCase (".lrc"))
        {
            loadTextFile (juce::File (f));
            return;
        }
}

void LyricsPanel::timerCallback()
{
    const double clock = engine.getClockSeconds();
    const int line = clock >= 0.0 ? data.lineAt (clock / engine.getBeatSeconds()) : -1;
    bool dirty = false;
    if (line != current)
    {
        current = line;
        dirty = true;
    }

    if (serverOk)
        server.publishLive (clock, engine.isPlaying(), line);

    if (juce::Time::currentTimeMillis() >= manualUntil)
    {
        const int follow = tapMode ? juce::jmax (0, tapIndex - 1) : current;
        float target = 0.0f;
        if (follow >= 0)
            target = juce::jmax (0.0f, (float) (follow * lineH + lineH / 2) - (float) viewArea().getHeight() * 0.35f);
        const float d = target - scrollY;
        if (std::abs (d) > 0.4f)
        {
            scrollY += d * 0.2f;
            dirty = true;
        }
        else if (d != 0.0f)
        {
            scrollY = target;
            dirty = true;
        }
    }

    if (dirty)
        repaint();
}

void LyricsPanel::paint (juce::Graphics& g)
{
    g.fillAll (Pal::window);

    // header
    g.setColour (Pal::surfaceLo);
    g.fillRect (getLocalBounds().withHeight (headerH));
    g.setColour (Pal::hairline);
    g.fillRect (0, headerH - 1, getWidth(), 1);

    g.setColour (Pal::text);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText (title.isEmpty() ? juce::String ("-") : title, 14, 8, getWidth() - 28, 24, juce::Justification::centredLeft, true);
    g.setColour (Pal::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (serverOk ? "Телефон: "_ru + url : "Страница для телефона не запущена (нет свободного порта)"_ru,
                14, 106, getWidth() - 28, 18, juce::Justification::centredLeft, true);

    const auto v = viewArea();
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (v);

        if (data.lines.isEmpty())
        {
            g.setColour (Pal::textDim);
            g.setFont (juce::FontOptions (16.0f));
            g.drawFittedText ("Текста нет. Нажмите «Загрузить...», вставьте из буфера или перетащите сюда файл .txt."_ru,
                              v.reduced (30, 40), juce::Justification::centredTop, 4);
        }

        const double bs = engine.getBeatSeconds();
        for (int i = 0; i < data.lines.size(); ++i)
        {
            const int y = v.getY() + 20 + i * lineH - (int) scrollY;
            if (y + lineH < v.getY() || y > v.getBottom())
                continue;

            const bool isCurrent = i == current;
            const bool past = current >= 0 && i < current;
            auto row = juce::Rectangle<int> (v.getX() + 34, y, v.getWidth() - 34 - 64, lineH);

            if (isCurrent)
            {
                g.setColour (Pal::accent);
                g.setFont (juce::FontOptions (28.0f, juce::Font::bold));
            }
            else
            {
                g.setColour (Pal::text.withAlpha (past ? 0.4f : 0.9f));
                g.setFont (juce::FontOptions (22.0f));
            }
            g.drawText (data.lines[i], row, juce::Justification::centredLeft, true);

            if (data.beats[(size_t) i] > LyricsData::none / 2)
            {
                const int secs = (int) std::round (juce::jmax (0.0, data.beats[(size_t) i] * bs));
                g.setColour (Pal::textDim);
                g.setFont (juce::FontOptions (12.0f));
                g.drawText (juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2),
                            v.getRight() - 62, y, 52, lineH, juce::Justification::centredRight);
            }

            if (tapMode && i == tapIndex)
            {
                juce::Path tri;
                const float cy = (float) y + lineH * 0.5f;
                tri.addTriangle (14.0f, cy - 8.0f, 14.0f, cy + 8.0f, 28.0f, cy);
                g.setColour (Pal::accent);
                g.fillPath (tri);
            }
        }
    }

    // hint line
    g.setColour (Pal::hairline);
    g.fillRect (0, getHeight() - 28, getWidth(), 1);
    g.setColour (tapMode ? Pal::accent : Pal::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (tapMode ? "РАЗМЕТКА: Enter - метка на отмеченной строке, Backspace - шаг назад, Esc - выйти"_ru
                        : "Двойной клик по строке с меткой - перейти к ней. Колесо - прокрутка."_ru,
                10, getHeight() - 26, getWidth() - 20, 24, juce::Justification::centredLeft, true);
}

// ---------------------------------------------------------------- window

LyricsWindow::LyricsWindow (Engine& e)
    : juce::DocumentWindow ("Текст песни"_ru, Pal::surface, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
{
    setUsingNativeTitleBar (true);
    panel = new LyricsPanel (e);
    panel->setSize (520, 760);
    setContentOwned (panel, true);
    setResizable (true, true);
    setResizeLimits (340, 420, 1400, 1800);
    centreWithSize (520, 760);
    setVisible (true);
}
