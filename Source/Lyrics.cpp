#include "Lyrics.h"
#include <map>

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

bool LyricsData::parseLrc (const juce::String& text, double beatSeconds, juce::String& plain, LyricsSections& out)
{
    juce::StringArray plainLines;
    std::vector<double> marks;
    bool any = false;

    for (auto& raw : juce::StringArray::fromLines (text))
    {
        auto line = raw.trimEnd();
        if (isLrcInfoTag (line.trim()))
            continue;

        double mark = -1.0;
        while (line.trimStart().startsWithChar ('['))
        {
            const auto t = line.trimStart();
            const int close = t.indexOfChar (']');
            double seconds = 0.0;
            if (close < 0 || ! parseTimeTag (t.substring (1, close), seconds))
                break;
            if (mark < 0.0)
                mark = seconds / juce::jmax (0.001, beatSeconds);
            line = t.substring (close + 1).trimStart();
        }
        any = any || mark >= 0.0;
        plainLines.add (line);
        marks.push_back (mark);
    }
    if (! any)
        return false;

    plain = plainLines.joinIntoString ("\n");
    out.clear();
    for (size_t i = 0; i < marks.size(); ++i)
    {
        if (marks[i] < 0.0)
            continue;
        double end = marks[i] + 16.0;
        for (size_t j = i + 1; j < marks.size(); ++j)
            if (marks[j] >= 0.0)
            {
                end = marks[j];
                break;
            }
        if (end - marks[i] >= LyricsSections::minLength)
            out.items.push_back ({ (int) i, (int) i + 1, marks[i], end });
    }
    std::sort (out.items.begin(), out.items.end(), [] (const LyricsSection& a, const LyricsSection& b) { return a.t0 < b.t0; });
    for (size_t i = 0; i + 1 < out.items.size(); ++i)
        out.items[i].t1 = std::min (out.items[i].t1, out.items[i + 1].t0);
    return true;
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
    text.clear();
    sections.clear();
    if (songFile == juce::File())
        return;

    const auto tf = textFile (songFile);
    if (! tf.existsAsFile())
        return;

    juce::MemoryBlock block;
    tf.loadFileAsData (block);
    text = decode (block).replace ("\r\n", "\n").replace ("\r", "\n");

    const auto mf = markFile (songFile);
    if (! mf.existsAsFile())
        return;

    const int numLines = juce::StringArray::fromLines (text).size();
    for (auto& row : juce::StringArray::fromLines (mf.loadFileAsString()))
    {
        const auto parts = juce::StringArray::fromTokens (row, " \t", {});
        if (parts.size() != 4)
            continue;   // older format or garbage
        LyricsSection s { parts[0].getIntValue(), parts[1].getIntValue(), parts[2].getDoubleValue(), parts[3].getDoubleValue() };
        if (s.first >= 0 && s.last > s.first && s.last <= numLines && s.t0 >= 0.0 && s.t1 - s.t0 >= LyricsSections::minLength)
            sections.items.push_back (s);
    }
    std::sort (sections.items.begin(), sections.items.end(), [] (const LyricsSection& a, const LyricsSection& b) { return a.t0 < b.t0; });
    for (size_t i = 0; i + 1 < sections.items.size(); ++i)
        sections.items[i].t1 = std::min (sections.items[i].t1, sections.items[i + 1].t0);
}

void LyricsData::save (const juce::File& songFile) const
{
    if (songFile == juce::File())
        return;

    const auto tf = textFile (songFile), mf = markFile (songFile);
    if (text.trim().isEmpty())
    {
        tf.deleteFile();
        mf.deleteFile();
        return;
    }

    tf.replaceWithText (text, false, false, "\n");
    if (sections.empty())
    {
        mf.deleteFile();
        return;
    }

    juce::StringArray rows;
    for (auto& s : sections.items)
        rows.add (juce::String (s.first) + " " + juce::String (s.last) + " " + juce::String (s.t0, 4) + " " + juce::String (s.t1, 4));
    mf.replaceWithText (rows.joinIntoString ("\n"), false, false, "\n");
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
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover,user-scalable=no">
<title>TC</title>
<style>
:root{--bg:#000;--panel:#161618;--raise:#2a2a2d;--text:#f2f2f7;--dim:#8e8e93;--acc:#ffa81f}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;height:100%;background:#000;color:var(--text);font-family:"Segoe UI",Roboto,-apple-system,"Helvetica Neue",Arial,sans-serif;-webkit-text-size-adjust:100%;overflow:hidden}
#stage{position:absolute;left:50%;top:50%;width:360px;height:660px;overflow:hidden;background:#000;transform-origin:50% 50%}
#top{position:absolute;left:0;right:0;top:0;height:58px;padding:9px 10px 9px 16px;background:var(--panel);border-bottom:2px solid var(--acc);display:flex;align-items:center;gap:10px}
#cur{flex:1;min-width:0}
#cur b{display:block;font-size:17px;color:var(--acc);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#cur small{display:block;font-size:12px;color:var(--dim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:2px}
#cur small i{font-style:normal;color:var(--text)}
#ib{flex:none;width:40px;height:40px;border:0;border-radius:11px;background:var(--raise);color:var(--text);padding:0}
#ib svg{width:22px;height:22px;fill:currentColor}
#w{position:absolute;left:0;right:0;top:58px;bottom:84px}
#t{position:absolute;left:0;right:0;top:0;bottom:0;padding:14px 16px 0;overflow:hidden;transition:opacity .15s}
#t.all{overflow-y:auto;scrollbar-width:none}
#t.out{opacity:0}
.ln{white-space:pre;font-size:18px;line-height:23px;font-weight:700;color:var(--text);margin-top:17px;text-align:left}
.ln:first-child{margin-top:0}
.ln.n{color:#5d5d62;font-weight:600}
.e{white-space:normal;color:var(--dim);font-weight:400;font-size:16px}
#b{position:absolute;left:0;right:0;bottom:0;height:84px;display:flex;gap:8px;padding:12px;background:var(--panel);border-top:1px solid #2c2c2e}
#b button{flex:1;border:0;border-radius:12px;background:var(--raise);color:var(--text);padding:0;display:flex;align-items:center;justify-content:center}
#b button svg{width:26px;height:26px;fill:currentColor}
#b button.on{background:var(--acc);color:#000}
#b button:active{filter:brightness(1.3)}
#pl{position:absolute;left:0;right:0;top:0;bottom:0;background:#000;z-index:5;display:none;flex-direction:column}
#pl.on{display:flex}
#ph{flex:none;display:flex;gap:8px;align-items:center;padding:9px 10px 9px 16px;height:58px;background:var(--panel);border-bottom:2px solid var(--acc)}
#ph select{flex:1;min-width:0;height:40px;font-size:15px;border:0;border-radius:11px;background:var(--raise);color:var(--text);padding:0 8px}
#ph button{flex:none;width:40px;height:40px;font-size:20px;border:0;border-radius:11px;background:var(--raise);color:var(--text);padding:0}
#pi{flex:1;overflow-y:auto;padding:8px 10px}
.r{display:flex;align-items:center;gap:4px;margin:5px 0;padding:0 0 0 12px;min-height:46px;border-radius:11px;background:var(--panel)}
.r.now{background:#2a1c05;outline:2px solid var(--acc)}
.r .t{flex:1;min-width:0;font-size:16px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.r.now .t{color:var(--acc);font-weight:700}
.r button{flex:none;width:40px;height:46px;font-size:16px;border:0;background:none;color:var(--dim)}
.r button:active{color:var(--acc)}
</style></head><body>
<div id="stage">
<div id="pl"><div id="ph"><select id="ls" onchange="cmd('list:'+this.value)"></select><button onclick="closePl()">&#10005;</button></div><div id="pi"></div></div>
<div id="top"><div id="cur"><b id="n">TC</b><small id="nx"></small></div>
<button id="ib" onclick="openPl()"><svg viewBox="0 0 24 24"><path d="M3 5h18v2.4H3zM3 10.8h18v2.4H3zM3 16.6h18V19H3z"/></svg></button></div>
<div id="w"><div id="t"></div></div>
<div id="b">
<button onclick="cmd('prev')"><svg viewBox="0 0 24 24"><path d="M5 5h2.4v14H5zM20 5v14L9 12z"/></svg></button>
<button id="bp" onclick="cmd('start')"><svg viewBox="0 0 24 24"><path d="M7 4.5v15L19.5 12z"/></svg></button>
<button id="bz" onclick="cmd('pause')"><svg viewBox="0 0 24 24"><path d="M6 5h4.2v14H6zM13.8 5H18v14h-4.2z"/></svg></button>
<button onclick="cmd('stop')"><svg viewBox="0 0 24 24"><path d="M6 6h12v12H6z"/></svg></button>
<button onclick="cmd('next')"><svg viewBox="0 0 24 24"><path d="M16.6 5H19v14h-2.4zM4 5v14l11-7z"/></svg></button>
</div>
</div>
<script>
// The page is a fixed 360 x 660 screen that is scaled to the phone, so it looks the same on every phone.
// Text: 18 px bold, 23 px rows, 17 px between lines of the song, left aligned. The rows are already cut by TC.
var rev=-1,plRev=-1,lines=[],secs=[],cur="",lastS=null,K=new URLSearchParams(location.search).get("k")||"";
var T=document.getElementById("t"),stage=document.getElementById("stage");
function layout(){var s=Math.min(innerWidth/360,innerHeight/660);stage.style.transform="translate(-50%,-50%) scale("+s+")"}
function cmd(c){fetch("/cmd?do="+encodeURIComponent(c)+"&k="+encodeURIComponent(K),{cache:"no-store"}).catch(function(){})}
function get(u){return fetch(u,{cache:"no-store"}).then(function(r){return r.json()})}
function openPl(){document.getElementById("pl").classList.add("on")}
function closePl(){document.getElementById("pl").classList.remove("on")}
function shown(i){return i<lines.length&&lines[i].trim()!==""}
function addLine(t,cls){var e=document.createElement("div");e.className="ln"+(cls?" "+cls:"");e.textContent=t;T.appendChild(e)}
function render(c,done){
  var key=c+"/"+done;if(key===cur)return;cur=key;
  var apply=function(){
    T.innerHTML="";T.classList.remove("all");T.classList.remove("out");
    if(!lines.length){var e=document.createElement("div");e.className="ln e";e.textContent="Текст не загружен";T.appendChild(e);return}
    if(!secs.length){T.classList.add("all");lines.forEach(function(x,i){if(shown(i))addLine(x)});return}
    var i=c>=0?c:Math.min(done,secs.length-1);
    var last=Math.min(secs[i][1],lines.length);
    for(var k=secs[i][0];k<last;k++)if(shown(k))addLine(lines[k],c>=0?"":"n");
    for(var k=last;k<lines.length;k++)if(shown(k)){addLine(lines[k].split("\n")[0],"n");break}
    var els=T.querySelectorAll(".ln");
    for(var j=0;j<els.length;j++){var w=els[j].scrollWidth;if(w>328)els[j].style.fontSize=(18*328/w)+"px"}
  };
  if(T.firstChild&&secs.length){T.classList.add("out");setTimeout(apply,120)}else apply();
}
function build(d){lines=d.lines||[];secs=d.secs||[];cur="";document.getElementById("n").textContent=d.title||"TC"}
function drawPl(d){
  var ls=document.getElementById("ls");ls.innerHTML="";
  (d.lists||[]).forEach(function(n){var o=document.createElement("option");o.value=n;o.textContent=n;if(n===d.name)o.selected=true;ls.appendChild(o)});
  var box=document.getElementById("pi");box.innerHTML="";
  (d.items||[]).forEach(function(n,i){
    var r=document.createElement("div");r.className="r"+(i===d.index?" now":"");
    var t=document.createElement("div");t.className="t";t.textContent=(i+1)+". "+n;t.onclick=function(){cmd("song:"+i);closePl()};
    var u=document.createElement("button");u.innerHTML="&#9650;";u.onclick=function(){cmd("up:"+i)};
    var dn=document.createElement("button");dn.innerHTML="&#9660;";dn.onclick=function(){cmd("down:"+i)};
    r.appendChild(t);r.appendChild(u);r.appendChild(dn);box.appendChild(r)});
}
function names(s){
  document.getElementById("n").textContent=s.cur||"TC";
  var nx=document.getElementById("nx");nx.innerHTML=s.nxt?"Далее: <i></i>":"";
  if(s.nxt)nx.querySelector("i").textContent=s.nxt;
}
function poll(){
  get("/state").then(function(s){
    lastS=s;
    document.getElementById("bp").className=s.playing?"on":"";
    document.getElementById("bz").className=s.paused?"on":"";
    names(s);
    if(s.pl!==plRev){plRev=s.pl;get("/playlist").then(drawPl)}
    if(s.rev!==rev){rev=s.rev;return get("/lyrics").then(build).then(function(){names(s);render(s.sec,s.done)})}
    render(s.sec,s.done);
  }).catch(function(){});
}
addEventListener("resize",layout);layout();
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
    // home Wi-Fi / router addresses first; 172.x is usually a virtual adapter (Hyper-V, WSL, Docker), 10.x often a VPN
    juce::String best[4];   // 192.168, 10, 172, anything else
    for (auto& a : juce::IPAddress::getAllAddresses())
    {
        const auto s = a.toString();
        if (s == "127.0.0.1" || s.startsWith ("169.254.") || s.contains (":"))
            continue;
        const int slot = s.startsWith ("192.168.") ? 0 : s.startsWith ("10.") ? 1 : s.startsWith ("172.") ? 2 : 3;
        if (best[slot].isEmpty())
            best[slot] = s;
    }
    for (auto& s : best)
        if (s.isNotEmpty())
            return s;
    return "127.0.0.1";
}

void LyricsServer::publishText (const juce::String& title, const juce::StringArray& lines, const std::vector<LyricsSection>& sections)
{
    auto* obj = new juce::DynamicObject();
    juce::Array<juce::var> arr, secs;
    for (auto& l : lines)
        arr.add (l);
    for (auto& s : sections)
        secs.add (juce::Array<juce::var> { s.first, s.last });
    obj->setProperty ("title", title);
    obj->setProperty ("lines", arr);
    obj->setProperty ("secs", secs);
    obj->setProperty ("empty", "Текст не загружен"_ru);

    const juce::ScopedLock sl (lock);
    lyricsJson = juce::JSON::toString (juce::var (obj), true);
    ++revision;
}

void LyricsServer::publishPlaylist (const juce::String& current, const juce::String& next, const juce::String& listJson)
{
    const juce::ScopedLock sl (lock);
    curName = current;
    nextName = next;
    if (listJson != playlistJson)
    {
        playlistJson = listJson;
        ++plRevision;
    }
}

void LyricsServer::publishLive (double seconds, bool playing, bool paused, int section, int finished, double bpm, int semitones)
{
    const juce::ScopedLock sl (lock);
    livePos = seconds;
    livePlaying = playing;
    livePaused = paused;
    liveSection = section;
    liveFinished = finished;
    liveBpm = bpm;
    liveSemis = semitones;
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
            json = "{\"rev\":" + juce::String (revision) + ",\"pl\":" + juce::String (plRevision) + ",\"pos\":" + juce::String (livePos, 2)
                 + ",\"playing\":" + (livePlaying ? "true" : "false") + ",\"paused\":" + (livePaused ? "true" : "false") + ",\"sec\":" + juce::String (liveSection) + ",\"done\":" + juce::String (liveFinished)
                 + ",\"bpm\":" + juce::String (liveBpm, 2) + ",\"semis\":" + juce::String (liveSemis)
                 + ",\"cur\":" + juce::JSON::toString (juce::var (curName)) + ",\"nxt\":" + juce::JSON::toString (juce::var (nextName)) + "}";
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
    else if (path == "/playlist")
    {
        juce::String json;
        {
            const juce::ScopedLock sl (lock);
            json = playlistJson.isEmpty() ? juce::String ("{\"items\":[],\"lists\":[]}") : playlistJson;
        }
        reply (client, "application/json", json);
    }
    else if (path == "/cmd")
    {
        const auto query = firstLine.fromFirstOccurrenceOf ("?", false, false).upToFirstOccurrenceOf (" ", false, false);
        juce::String command, given;
        for (auto& part : juce::StringArray::fromTokens (query, "&", {}))
        {
            if (part.startsWith ("do="))
                command = juce::URL::removeEscapeChars (part.substring (3));
            else if (part.startsWith ("k="))
                given = juce::URL::removeEscapeChars (part.substring (2));
        }
        const bool allowed = key.isNotEmpty() && given == key && command.isNotEmpty();
        if (allowed && onCommand)
            onCommand (command);
        reply (client, "text/plain", allowed ? "ok" : "no");
    }
    else if (path == "/" || path == "/index.html")
        reply (client, "text/html", juce::String::fromUTF8 (phonePage));
    else
    {
        const juce::String head = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        client.write (head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
    }
}


// ---------------------------------------------------------------- helpers

// The phone screen is a fixed 360 x 660 page (see the phone page): 18 px bold text, 23 px rows,
// 17 px between lines of the song. A block of the lyrics has to fit the text area together with the first row of what follows.
namespace phoneScreen
{
    constexpr float textWidth = 308.0f;   // 328 px of the page minus a reserve for the different fonts of the phones
    constexpr float rowHeight = 23.0f, lineGap = 17.0f, areaHeight = 494.0f;

    bool isTag (const juce::String& line)   // [Verse 1] and so on: not shown on the phone
    {
        const auto t = line.trim();
        return t.length() > 1 && t.startsWith ("[") && t.endsWith ("]");
    }

    float measure (const juce::String& text)
    {
        static const juce::Font font (juce::FontOptions ("Segoe UI", 18.0f, juce::Font::bold));
        juce::GlyphArrangement ga;
        ga.addLineOfText (font, text, 0.0f, 0.0f);
        return ga.getBoundingBox (0, -1, true).getWidth();
    }

    // the rows of one line of the song: cut by words and balanced, so that no row is left with one word
    juce::StringArray cut (const juce::String& line)
    {
        juce::StringArray out;
        const auto trimmed = line.trim();
        if (trimmed.isEmpty() || isTag (trimmed))
            return out;

        const auto words = juce::StringArray::fromTokens (trimmed, " \t", "");
        auto greedy = [&words] (float limit, juce::StringArray* rows)
        {
            int count = 0;
            juce::String current;
            for (auto& w : words)
            {
                const auto test = current.isEmpty() ? w : current + " " + w;
                if (current.isNotEmpty() && measure (test) > limit)
                {
                    if (rows != nullptr)
                        rows->add (current);
                    current = w;
                    ++count;
                }
                else
                    current = test;
            }
            if (current.isNotEmpty())
            {
                if (rows != nullptr)
                    rows->add (current);
                ++count;
            }
            return count;
        };

        const int n = greedy (textWidth, nullptr);
        if (n <= 1)
        {
            out.add (words.joinIntoString (" "));
            return out;
        }
        float lo = 0.0f, hi = textWidth;   // the narrowest limit that still gives the same number of rows
        for (int i = 0; i < 14; ++i)
        {
            const float mid = 0.5f * (lo + hi);
            if (greedy (mid, nullptr) <= n)
                hi = mid;
            else
                lo = mid;
        }
        greedy (hi, &out);
        return out;
    }

    juce::StringArray cutCached (const juce::String& line)
    {
        static std::map<juce::String, juce::StringArray> cache;
        if (cache.size() > 3000)
            cache.clear();
        auto it = cache.find (line);
        if (it == cache.end())
            it = cache.emplace (line, cut (line)).first;
        return it->second;
    }
}

namespace
{
    juce::String formatTime (double seconds)
    {
        const bool negative = seconds < 0.0;
        seconds = std::abs (seconds);
        const int whole = (int) seconds;
        const int tenth = (int) ((seconds - whole) * 10.0);
        return (negative ? "-" : "") + juce::String (whole / 60) + ":" + juce::String (whole % 60).paddedLeft ('0', 2) + "." + juce::String (tenth);
    }

    juce::Colour sectionColour (int index)
    {
        return index % 2 == 0 ? Pal::clip : Pal::cyan;
    }

    // vertical curly brace "{": the tip looks to the left
    void addVerticalBrace (juce::Path& p, float xRight, float yTop, float yBottom, float width)
    {
        const float ym = (yTop + yBottom) * 0.5f;
        const float r = juce::jmin (width * 0.5f, (yBottom - yTop) * 0.25f);
        const float xl = xRight - width, xm = xRight - width * 0.5f;
        p.startNewSubPath (xRight, yTop);
        p.quadraticTo (xm, yTop, xm, yTop + r);
        p.lineTo (xm, ym - r);
        p.quadraticTo (xm, ym, xl, ym);
        p.quadraticTo (xm, ym, xm, ym + r);
        p.lineTo (xm, yBottom - r);
        p.quadraticTo (xm, yBottom, xRight, yBottom);
    }

    // horizontal curly brace: the tip looks up
    void addHorizontalBrace (juce::Path& p, float xLeft, float xRight, float yTop, float yBottom)
    {
        const float xm = (xLeft + xRight) * 0.5f;
        const float h = yBottom - yTop;
        const float r = juce::jmin (h * 0.5f, (xRight - xLeft) * 0.25f);
        const float ym = yTop + h * 0.5f;
        p.startNewSubPath (xLeft, yBottom);
        p.quadraticTo (xLeft, ym, xLeft + r, ym);
        p.lineTo (xm - r, ym);
        p.quadraticTo (xm, ym, xm, yTop);
        p.quadraticTo (xm, ym, xm + r, ym);
        p.lineTo (xRight - r, ym);
        p.quadraticTo (xRight, ym, xRight, yBottom);
    }
}

// ---------------------------------------------------------------- overlay over the text: dividing line + braces

class LyricsPanel::Overlay : public juce::Component
{
public:
    explicit Overlay (LyricsPanel& o) : owner (o) {}

    bool hitTest (int x, int y) override
    {
        return x < gutterW || hitAt ({ x, y }).zone != Zone::None;
    }

    void paint (juce::Graphics& g) override
    {
        owner.updateGeometry();
        const int n = owner.lines.size();
        if (n == 0)
            return;

        const float right = (float) getWidth();
        const auto& items = owner.data.sections.items;

        // blocks that are already on the timeline
        for (int i = 0; i < (int) items.size(); ++i)
        {
            const auto& s = items[(size_t) i];
            const float y0 = owner.lineY (s.first), y1 = owner.lineY (s.last);
            const bool playing = i == owner.playingSection;
            const bool selected = i == owner.selected;
            g.setColour (playing ? Pal::accent.withAlpha (0.20f) : sectionColour (i).withAlpha (selected ? 0.20f : 0.10f));
            g.fillRect (juce::Rectangle<float> ((float) gutterW, y0, right - gutterW, y1 - y0));
        }

        // the block that "+" would take now
        const int pf = owner.pendingFirst();
        if (owner.divider > pf)
        {
            g.setColour (Pal::accent.withAlpha (0.16f));
            g.fillRect (juce::Rectangle<float> ((float) gutterW, owner.lineY (pf), right - gutterW, owner.lineY (owner.divider) - owner.lineY (pf)));
        }

        // braces and numbers
        for (int i = 0; i < (int) items.size(); ++i)
        {
            const auto& s = items[(size_t) i];
            const float y0 = owner.lineY (s.first) + 2.0f, y1 = owner.lineY (s.last) - 2.0f;
            if (y1 <= y0)
                continue;
            const bool playing = i == owner.playingSection;
            const bool selected = i == owner.selected;

            juce::Path brace;
            addVerticalBrace (brace, (float) braceX1, y0, y1, (float) (braceX1 - braceX0));
            const bool tooBig = ! owner.fits (s.first, s.last);   // does not fit the phone screen
            g.setColour (tooBig ? juce::Colour (0xffff5a4f) : playing || selected ? Pal::accent : sectionColour (i).brighter (0.2f));
            g.strokePath (brace, juce::PathStrokeType (selected ? 3.0f : 2.0f));

            g.setColour (selected ? Pal::accent : Pal::textDim);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (juce::String (i + 1), 2, (int) ((y0 + y1) * 0.5f) - 8, braceX0 - 4, 16, juce::Justification::centredRight);

            // text joint with the next block: both braces meet
            if (owner.data.sections.textTouchesNext (i))
            {
                g.setColour (Pal::accent.withAlpha (0.8f));
                g.fillRect (juce::Rectangle<float> ((float) braceX0 + 3.0f, owner.lineY (s.last) - 1.0f, (float) (braceX1 - braceX0) - 2.0f, 2.0f));
            }
        }

        // the dividing line
        const float dy = owner.lineY (owner.divider);
        g.setColour (Pal::accent);
        const float dashes[] = { 7.0f, 4.0f };
        g.drawDashedLine (juce::Line<float> ((float) gutterW - 8.0f, dy, right, dy), dashes, 2, 2.0f);
        auto grip = juce::Rectangle<float> (4.0f, dy - 9.0f, (float) gutterW - 14.0f, 18.0f);
        g.fillRoundedRectangle (grip, 5.0f);
        g.setColour (Pal::onAccent);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (juce::String (juce::jmax (0, owner.divider - pf)), grip.toNearestInt(), juce::Justification::centred);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto hit = hitAt (e.getPosition());
        setMouseCursor (hit.zone == Zone::Divider || hit.zone == Zone::BraceTop || hit.zone == Zone::BraceBottom
                            ? juce::MouseCursor::UpDownResizeCursor
                            : hit.zone == Zone::BraceBody ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        press = hitAt (e.getPosition());
        startY = e.y;
        applied = 0;
        if (press.zone == Zone::Divider)
            return;
        if (press.zone == Zone::None)
        {
            owner.select (-1);
            return;
        }
        owner.select (press.index);
        if (e.mods.isPopupMenu())
        {
            press.zone = Zone::None;
            owner.showDeleteMenu (press.index);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto& sections = owner.data.sections;
        const int n = owner.lines.size();
        const auto before = sections.items;
        switch (press.zone)
        {
            case Zone::Divider:
                owner.setDivider (owner.boundaryAtY ((float) e.y));
                return;
            case Zone::BraceTop:
                sections.moveFirst (press.index, owner.boundaryAtY ((float) e.y));
                break;
            case Zone::BraceBottom:
                sections.moveLast (press.index, owner.boundaryAtY ((float) e.y), n);
                break;
            case Zone::BraceBody:
            {
                const int wanted = (int) std::round ((e.y - startY) / owner.geo.pitch);
                const int before = sections.items[(size_t) press.index].first;
                sections.shiftLines (press.index, wanted - applied, n);
                applied += sections.items[(size_t) press.index].first - before;
                break;
            }
            case Zone::None:
                return;
        }

        // a block may not grow past the phone screen
        for (size_t i = 0; i < sections.items.size() && i < before.size(); ++i)
        {
            const auto& a = sections.items[i];
            const auto& b = before[i];
            if ((a.first != b.first || a.last != b.last)
                && owner.blockHeight (a.first, a.last) > owner.blockHeight (b.first, b.last) + 0.1f
                && ! owner.fits (a.first, a.last))
            {
                sections.items = before;
                owner.say ("Не помещается на экран телефона"_ru);
                return;
            }
        }
        owner.sectionsChanged();
    }

    void mouseUp (const juce::MouseEvent&) override { press = {}; }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        owner.editor.mouseWheelMove (e.getEventRelativeTo (&owner.editor), w);
        repaint();
    }

private:
    enum class Zone { None, Divider, BraceTop, BraceBottom, BraceBody };
    struct Hit { Zone zone = Zone::None; int index = -1; };

    static constexpr int braceX0 = gutterW - 36, braceX1 = gutterW - 6;

    Hit hitAt (juce::Point<int> p) const
    {
        owner.updateGeometry();
        if (owner.lines.isEmpty())
            return {};

        const float dy = owner.lineY (owner.divider);
        if (std::abs ((float) p.y - dy) <= (p.x < gutterW ? 10.0f : 3.0f))
            return { Zone::Divider, -1 };

        if (p.x >= braceX0 - 4 && p.x <= braceX1 + 2)
        {
            const auto& items = owner.data.sections.items;
            for (int i = (int) items.size() - 1; i >= 0; --i)
            {
                const float y0 = owner.lineY (items[(size_t) i].first), y1 = owner.lineY (items[(size_t) i].last);
                if ((float) p.y < y0 || (float) p.y > y1)
                    continue;
                if ((float) p.y < y0 + 8.0f)
                    return { Zone::BraceTop, i };
                if ((float) p.y > y1 - 8.0f)
                    return { Zone::BraceBottom, i };
                return { Zone::BraceBody, i };
            }
        }
        return {};
    }

    LyricsPanel& owner;
    Hit press;
    int startY = 0, applied = 0;
};

// ---------------------------------------------------------------- the timeline

class LyricsPanel::TimelineView : public juce::Component
{
public:
    explicit TimelineView (LyricsPanel& o) : owner (o) { setWantsKeyboardFocus (true); }

    static constexpr int rulerH = 26;
    double pxPerSec = 30.0, viewStart = 0.0;
    juce::uint32 manualUntil = 0;

    void followPlayhead (double seconds)
    {
        if (juce::Time::getMillisecondCounter() < manualUntil)
            return;
        const float x = xOf (seconds);
        if (x < 0.0f || x > (float) getWidth() * 0.9f)
            viewStart = juce::jmax (0.0, seconds - 0.1 * getWidth() / pxPerSec);
    }

    void paint (juce::Graphics& g) override
    {
        const int w = getWidth(), h = getHeight();
        const double bs = owner.beatSeconds();
        g.fillAll (Pal::sunken);

        // ruler
        g.setColour (Pal::surfaceLo);
        g.fillRect (0, 0, w, rulerH);
        double step = 1.0;
        for (double s : { 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 })
        {
            step = s;
            if (s * pxPerSec >= 70.0)
                break;
        }
        g.setFont (juce::FontOptions (11.0f));
        for (double t = std::floor (viewStart / step) * step; xOf (t) < (float) w; t += step)
        {
            const float x = xOf (t);
            if (x < 0.0f)
                continue;
            g.setColour (Pal::hairline);
            g.drawVerticalLine ((int) x, (float) rulerH - 8.0f, (float) h);
            g.setColour (Pal::textDim);
            g.drawText (formatTime (t), (int) x + 3, 4, 60, 14, juce::Justification::centredLeft);
        }
        g.setColour (Pal::hairline);
        g.fillRect (0, rulerH - 1, w, 1);

        // sections: a brace over a tinted bar with the first words
        const auto& items = owner.data.sections.items;
        const float bandTop = (float) rulerH + 6.0f, bandBottom = (float) h - 6.0f;
        for (int i = 0; i < (int) items.size(); ++i)
        {
            const auto& s = items[(size_t) i];
            const float x0 = xOf (s.t0 * bs), x1 = xOf (s.t1 * bs);
            if (x1 < 0.0f || x0 > (float) w)
                continue;
            const bool selected = i == owner.selected, playing = i == owner.playingSection;
            const auto colour = playing || selected ? Pal::accent : sectionColour (i).brighter (0.2f);

            auto bar = juce::Rectangle<float> (x0 + 1.0f, bandTop + 20.0f, juce::jmax (2.0f, x1 - x0 - 2.0f), bandBottom - bandTop - 20.0f);
            g.setColour (colour.withAlpha (selected ? 0.30f : 0.16f));
            g.fillRoundedRectangle (bar, 5.0f);

            juce::Path brace;
            addHorizontalBrace (brace, x0 + 1.0f, x1 - 1.0f, bandTop, bandTop + 16.0f);
            g.setColour (colour);
            g.strokePath (brace, juce::PathStrokeType (selected ? 3.0f : 2.0f));

            juce::String label = juce::String (i + 1) + "  ";
            for (int k = s.first; k < s.last && k < owner.lines.size(); ++k)
                if (owner.lines[k].trim().isNotEmpty())
                {
                    label += owner.lines[k].trim();
                    break;
                }
            label += "  (" + juce::String (s.last - s.first) + ")";
            {
                juce::Graphics::ScopedSaveState state (g);
                g.reduceClipRegion (bar.toNearestInt().reduced (4, 0));
                g.setColour (Pal::text);
                g.setFont (juce::FontOptions (13.0f));
                g.drawText (label, bar.toNearestInt().reduced (6, 2), juce::Justification::topLeft, false);
            }

            // two sections that touch move together: shown by a bright joint
            if (owner.data.sections.touchesNext (i))
            {
                g.setColour (Pal::accent);
                g.fillRect (juce::Rectangle<float> (x1 - 1.5f, bandTop, 3.0f, bandBottom - bandTop));
            }
        }

        // playhead
        const float px = xOf (owner.engine.getClockSeconds());
        if (px >= 0.0f && px <= (float) w)
        {
            g.setColour (Pal::red);
            g.fillRect (px - 1.0f, 0.0f, 2.0f, (float) h);
            juce::Path tri;
            tri.addTriangle (px - 6.0f, 0.0f, px + 6.0f, 0.0f, px, 9.0f);
            g.fillPath (tri);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (e.y > rulerH && edgeAt (e.x).index >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        auto& sections = owner.data.sections;
        mode = Mode::None;

        if (e.mods.isPopupMenu())
        {
            const int i = sectionAtX (e.x);
            if (i >= 0)
            {
                owner.select (i);
                owner.showDeleteMenu (i);
            }
            return;
        }
        if (e.y < rulerH)
        {
            mode = Mode::Scrub;
            owner.seekSeconds (juce::jmax (0.0, secAt ((float) e.x)));
            return;
        }

        const auto edge = edgeAt (e.x);
        if (edge.index >= 0)
        {
            owner.select (edge.index);
            index = edge.index;
            mode = edge.isEnd ? Mode::EndEdge : Mode::StartEdge;
            return;
        }

        const int i = sectionAtX (e.x);
        owner.select (i);
        if (i >= 0)
        {
            index = i;
            mode = Mode::Body;
            grabSec = secAt ((float) e.x);
            grabStart = sections.items[(size_t) i].t0;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto& sections = owner.data.sections;
        const double bs = owner.beatSeconds();
        switch (mode)
        {
            case Mode::Scrub:
                owner.seekSeconds (juce::jmax (0.0, secAt ((float) e.x)));
                return;
            case Mode::StartEdge:
                sections.moveStart (index, secAt ((float) e.x) / bs);
                break;
            case Mode::EndEdge:
                sections.moveEnd (index, secAt ((float) e.x) / bs, owner.maxBeats());
                break;
            case Mode::Body:
                sections.moveBody (index, grabStart + (secAt ((float) e.x) - grabSec) / bs - sections.items[(size_t) index].t0, owner.maxBeats());
                break;
            case Mode::None:
                return;
        }
        owner.sectionsChanged();
    }

    void mouseUp (const juce::MouseEvent&) override { mode = Mode::None; }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int i = e.y > rulerH ? sectionAtX (e.x) : -1;
        if (i >= 0)
            owner.seekSeconds (owner.data.sections.items[(size_t) i].t0 * owner.beatSeconds());
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        manualUntil = juce::Time::getMillisecondCounter() + 3000;
        if (e.mods.isCtrlDown() || e.mods.isCommandDown())
        {
            const double anchor = secAt ((float) e.x);
            pxPerSec = juce::jlimit (4.0, 400.0, pxPerSec * std::pow (1.6, (double) w.deltaY * 2.0));
            viewStart = juce::jmax (0.0, anchor - e.x / pxPerSec);
        }
        else
        {
            viewStart = juce::jmax (0.0, viewStart - (double) (w.deltaY + w.deltaX) * 300.0 / pxPerSec);
        }
        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            owner.deleteSelected();
            return true;
        }
        return false;
    }

private:
    enum class Mode { None, Scrub, StartEdge, EndEdge, Body };
    struct Edge { int index = -1; bool isEnd = false; };

    float xOf (double sec) const { return (float) ((sec - viewStart) * pxPerSec); }
    double secAt (float x) const { return viewStart + (double) x / pxPerSec; }

    int sectionAtX (int x) const
    {
        const double bs = owner.beatSeconds();
        const auto& items = owner.data.sections.items;
        for (int i = (int) items.size() - 1; i >= 0; --i)
            if (x >= xOf (items[(size_t) i].t0 * bs) && x <= xOf (items[(size_t) i].t1 * bs))
                return i;
        return -1;
    }

    Edge edgeAt (int x) const
    {
        const double bs = owner.beatSeconds();
        const auto& items = owner.data.sections.items;
        Edge best;
        float bestDistance = 7.0f;
        for (int i = 0; i < (int) items.size(); ++i)
        {
            const float d0 = std::abs ((float) x - xOf (items[(size_t) i].t0 * bs));
            const float d1 = std::abs ((float) x - xOf (items[(size_t) i].t1 * bs));
            if (d0 < bestDistance)
            {
                bestDistance = d0;
                best = { i, false };
            }
            if (d1 < bestDistance)
            {
                bestDistance = d1;
                best = { i, true };
            }
        }
        return best;
    }

    LyricsPanel& owner;
    Mode mode = Mode::None;
    int index = -1;
    double grabSec = 0.0, grabStart = 0.0;
};

// ---------------------------------------------------------------- panel

LyricsPanel::LyricsPanel (Engine& e) : engine (e)
{
    // the phone must know this key to press the buttons (kept between runs, so a bookmark keeps working)
    auto keyFile = engine.getDataDirectory().getChildFile ("phone-key.txt");
    auto key = keyFile.loadFileAsString().trim();
    if (key.length() < 4)
    {
        key = juce::String (juce::Random::getSystemRandom().nextInt (900000) + 100000);
        keyFile.replaceWithText (key);
    }
    server.setKey (key);
    juce::Component::SafePointer<LyricsPanel> safeThis (this);
    server.onCommand = [safeThis] (const juce::String& command)
    {
        juce::MessageManager::callAsync ([safeThis, command] { if (safeThis != nullptr && safeThis->onRemote) safeThis->onRemote (command); });
    };
    serverOk = server.start();
    if (serverOk)
        url = "http://" + LyricsServer::findLocalAddress() + ":" + juce::String (server.getPort()) + "/?k=" + key;

    loadButton.setButtonText ("Загрузить текст..."_ru);
    loadButton.setTooltip ("Загрузить текст из файла (.txt, .lrc). Текст можно и просто вставить в поле (ПКМ - Вставить)."_ru);
    loadButton.onClick = [this] { chooseFile(); };
    phoneButton.setButtonText ("Адрес для телефона"_ru);
    phoneButton.setTooltip ("Скопировать адрес страницы для телефона (телефон и ПК в одной сети)"_ru);
    phoneButton.onClick = [this]
    {
        if (url.isEmpty())
            return;
        juce::SystemClipboard::copyTextToClipboard (url);
        say ("Адрес скопирован: "_ru + url);
    };
    plusButton.setButtonText ("+");
    plusButton.setTooltip ("Поставить маркер: выделенные строки ложатся на таймлайн в текущий момент (F9)"_ru);
    plusButton.setColour (juce::TextButton::buttonColourId, Pal::accent);
    plusButton.setColour (juce::TextButton::textColourOffId, Pal::onAccent);
    plusButton.onClick = [this] { addMarker(); };
    deleteButton.setButtonText ("Удалить маркер"_ru);
    deleteButton.setTooltip ("Удалить выбранный маркер (текст остаётся)"_ru);
    deleteButton.onClick = [this] { deleteSelected(); };

    countLabel.setText ("Строк в блоке:"_ru, juce::dontSendNotification);
    countLabel.setColour (juce::Label::textColourId, Pal::textDim);
    countSlider.setSliderStyle (juce::Slider::IncDecButtons);
    countSlider.setRange (1.0, 64.0, 1.0);
    countSlider.setValue (count, juce::dontSendNotification);
    countSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 28);
    countSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsNotDraggable);
    countSlider.setTooltip ("Сколько строк берёт один маркер. Разделительную строку можно и перетащить мышью."_ru);
    countSlider.onValueChange = [this]
    {
        count = (int) countSlider.getValue();
        resetDivider();
        overlay->repaint();
    };

    infoLabel.setColour (juce::Label::textColourId, Pal::textDim);
    infoLabel.setFont (juce::FontOptions (13.0f));
    timeLabel.setColour (juce::Label::textColourId, Pal::text);
    timeLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    timeLabel.setJustificationType (juce::Justification::centred);
    hintLabel.setColour (juce::Label::textColourId, Pal::textDim);
    hintLabel.setFont (juce::FontOptions (12.0f));

    playButton.setTooltip ("Играть (F8 - играть / пауза)"_ru);
    playButton.onClick = [this] { engine.playAll(); };
    pauseButton.setTooltip ("Пауза"_ru);
    pauseButton.onClick = [this] { engine.pauseAll(); };
    stopButton.setTooltip ("Стоп, в начало"_ru);
    stopButton.onClick = [this] { engine.stopAll(); };

    editor.setMultiLine (true, false);
    editor.setReturnKeyStartsNewLine (true);
    editor.setTabKeyUsedAsCharacter (false);
    editor.setPopupMenuEnabled (true);
    editor.setScrollbarsShown (true);
    editor.setFont (juce::Font (juce::FontOptions (21.0f)));
    editor.setColour (juce::TextEditor::backgroundColourId, Pal::sunken);
    editor.setColour (juce::TextEditor::textColourId, Pal::text);
    editor.setColour (juce::TextEditor::outlineColourId, Pal::hairline);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, Pal::accent.withAlpha (0.6f));
    editor.setColour (juce::TextEditor::highlightColourId, Pal::accent.withAlpha (0.35f));
    editor.setColour (juce::TextEditor::highlightedTextColourId, Pal::text);
    editor.setColour (juce::CaretComponent::caretColourId, Pal::accent);
    editor.addListener (this);

    overlay = std::make_unique<Overlay> (*this);
    timeline = std::make_unique<TimelineView> (*this);

    editorArea.addAndMakeVisible (editor);
    editorArea.addAndMakeVisible (*overlay);
    for (auto* c : std::initializer_list<juce::Component*> { &loadButton, &phoneButton, &plusButton, &deleteButton, &countLabel, &countSlider,
                                                              &infoLabel, &timeLabel, &hintLabel, &playButton, &pauseButton, &stopButton,
                                                              &editorArea, timeline.get() })
        addAndMakeVisible (c);

    for (auto* b : std::initializer_list<juce::Component*> { &loadButton, &phoneButton, &plusButton, &deleteButton, &countSlider,
                                                              &playButton, &pauseButton, &stopButton })
    {
        b->setWantsKeyboardFocus (false);
        b->setMouseClickGrabsKeyboardFocus (false);
    }

    setWantsKeyboardFocus (false);
    startTimerHz (30);
}

LyricsPanel::~LyricsPanel()
{
    stopTimer();
    editor.removeListener (this);
    saveNow();
}

void LyricsPanel::paint (juce::Graphics& g)
{
    g.fillAll (Pal::window);
    g.setColour (Pal::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (serverOk ? "Телефон: "_ru + url : "Страница для телефона не запущена (нет свободного порта)"_ru,
                getLocalBounds().reduced (12, 0).removeFromTop (44).withTrimmedLeft (380), juce::Justification::centredRight, true);
}

void LyricsPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 8);

    auto top = r.removeFromTop (32);
    loadButton.setBounds (top.removeFromLeft (170));
    top.removeFromLeft (8);
    phoneButton.setBounds (top.removeFromLeft (170));
    r.removeFromTop (8);

    hintLabel.setBounds (r.removeFromBottom (20));
    auto timelineArea = r.removeFromBottom (150);
    r.removeFromBottom (8);
    auto strip = r.removeFromBottom (44);
    r.removeFromBottom (8);

    editorArea.setBounds (r);
    editor.setBounds (gutterW, 0, editorArea.getWidth() - gutterW, editorArea.getHeight());
    overlay->setBounds (editorArea.getLocalBounds());

    countLabel.setBounds (strip.removeFromLeft (112));
    countSlider.setBounds (strip.removeFromLeft (112));
    strip.removeFromLeft (12);
    plusButton.setBounds (strip.removeFromLeft (150));
    strip.removeFromLeft (8);
    deleteButton.setBounds (strip.removeFromLeft (150));
    strip.removeFromLeft (12);
    infoLabel.setBounds (strip);

    auto transport = timelineArea.removeFromLeft (118);
    timelineArea.removeFromLeft (8);
    auto buttons = transport.removeFromTop (34);
    const int bw = (buttons.getWidth() - 8) / 3;
    playButton.setBounds (buttons.removeFromLeft (bw));
    buttons.removeFromLeft (4);
    pauseButton.setBounds (buttons.removeFromLeft (bw));
    buttons.removeFromLeft (4);
    stopButton.setBounds (buttons);
    transport.removeFromTop (6);
    timeLabel.setBounds (transport.removeFromTop (30));
    timeline->setBounds (timelineArea);
}

bool LyricsPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::F9Key)
    {
        addMarker();
        return true;
    }
    if (key == juce::KeyPress::F8Key)
    {
        if (engine.isPlaying() && ! engine.isPaused())
            engine.pauseAll();
        else
            engine.playAll();
        return true;
    }
    return false;
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
            juce::MemoryBlock block;
            if (juce::File (f).loadFileAsData (block))
                loadText (LyricsData::decode (block));
            return;
        }
}

// ---- text and geometry

void LyricsPanel::refreshLines()
{
    lines = juce::StringArray::fromLines (editor.getText());
    lineStarts.clear();
    int start = 0;
    for (auto& l : lines)
    {
        lineStarts.push_back (start);
        start += l.length() + 1;
    }

    // rows of every line on the phone screen
    wrapped.clear();
    rowCount.clear();
    for (auto& l : lines)
    {
        const auto rows = phoneScreen::cutCached (l);
        rowCount.push_back (rows.size());
        wrapped.add (rows.joinIntoString ("\n"));
    }
}

float LyricsPanel::blockHeight (int first, int last) const
{
    float h = 0.0f;
    int shown = 0;
    for (int i = juce::jmax (0, first); i < juce::jmin (last, (int) rowCount.size()); ++i)
        if (rowCount[(size_t) i] > 0)
        {
            h += (float) rowCount[(size_t) i] * phoneScreen::rowHeight + (shown > 0 ? phoneScreen::lineGap : 0.0f);
            ++shown;
        }
    return h;
}

bool LyricsPanel::fits (int first, int last) const
{
    float h = blockHeight (first, last);
    for (int i = juce::jmax (0, last); i < (int) rowCount.size(); ++i)
        if (rowCount[(size_t) i] > 0)
        {
            h += phoneScreen::lineGap + phoneScreen::rowHeight;   // the first row of what follows is shown under the block
            break;
        }
    return h <= phoneScreen::areaHeight + 0.5f;
}

int LyricsPanel::maxDivider (int firstLine) const
{
    int d = juce::jmin (lines.size(), firstLine + 1);
    for (int e = firstLine + 1; e <= lines.size(); ++e)
    {
        if (! fits (firstLine, e))
            break;
        d = e;
    }
    return d;
}

void LyricsPanel::updateGeometry()
{
    int a = -1, b = -1;
    for (int i = 0; i < lines.size(); ++i)
        if (lines[i].trim().isNotEmpty())
        {
            if (a < 0)
                a = i;
            b = i;
        }
    if (a < 0)
        return;

    auto yOf = [this] (int line, float& y, float& h)
    {
        const auto rects = editor.getTextBounds ({ lineStarts[(size_t) line], lineStarts[(size_t) line] + 1 });
        if (rects.isEmpty())
            return false;
        const auto box = rects.getBounds();
        y = (float) box.getY();
        h = (float) box.getHeight();
        return true;
    };

    float ya = 0.0f, ha = 0.0f, yb = 0.0f, hb = 0.0f;
    if (! yOf (a, ya, ha))
        return;
    float pitch = ha;
    if (b != a && yOf (b, yb, hb))
        pitch = (yb - ya) / (float) (b - a);
    geo.pitch = juce::jmax (8.0f, pitch);
    geo.top0 = ya - (float) a * geo.pitch + (float) editor.getY();
}

int LyricsPanel::pendingFirst() const
{
    int f = data.sections.linesEnd();
    while (f < lines.size() && lines[f].trim().isEmpty())
        ++f;
    return juce::jmin (f, lines.size());
}

void LyricsPanel::resetDivider()
{
    const int pf = pendingFirst();
    divider = juce::jmin (juce::jmin (lines.size(), pf + count), maxDivider (pf));   // not more than the phone screen takes
    countSlider.setValue (juce::jmax (1, divider - pf), juce::dontSendNotification);
}

void LyricsPanel::setDivider (int boundary)
{
    const int pf = pendingFirst();
    const int limit = maxDivider (pf);
    if (boundary > limit && limit < lines.size())
        say ("Больше не помещается на экран телефона"_ru);
    divider = juce::jlimit (pf, juce::jmax (pf, limit), boundary);
    count = juce::jmax (1, divider - pf);
    countSlider.setValue (count, juce::dontSendNotification);
    overlay->repaint();
}

void LyricsPanel::textEditorTextChanged (juce::TextEditor&)
{
    const auto old = lines;
    refreshLines();

    int kept = 0;
    while (kept < old.size() && kept < lines.size() && old[kept] == lines[kept])
        ++kept;
    const int delta = lines.size() - old.size();
    if (delta != 0)
        data.sections.textEdited (kept, delta, lines.size());
    if (selected >= data.sections.size())
        selected = -1;

    data.text = editor.getText();
    if (old.size() <= 1 && lines.size() > 1)
        resetDivider();                       // the text just appeared: the dividing line goes to its place
    else
        divider = juce::jlimit (pendingFirst(), juce::jmax (pendingFirst(), juce::jmin (lines.size(), maxDivider (pendingFirst()))), divider);
    dirty = true;
    dirtyAt = juce::Time::getMillisecondCounter();
    overlay->repaint();
    timeline->repaint();
}

// ---- sections

double LyricsPanel::maxBeats() const
{
    double end = engine.getSongEnd();
    for (auto& s : data.sections.items)
        end = juce::jmax (end, s.t1 * beatSeconds());
    return (end + 30.0) / beatSeconds();
}

void LyricsPanel::seekSeconds (double seconds)
{
    if (onSeek)
        onSeek (seconds);
    else
        engine.seek (seconds);
}

void LyricsPanel::say (const juce::String& text)
{
    statusText = text;
    statusUntil = juce::Time::getMillisecondCounter() + 4000;
}

void LyricsPanel::select (int index)
{
    selected = index >= 0 && index < data.sections.size() ? index : -1;
    overlay->repaint();
    timeline->repaint();
}

void LyricsPanel::addMarker()
{
    const double clock = engine.getClockSeconds();
    if (clock < 0.0)
    {
        say ("Идёт отсчёт: маркер ставится после первой доли"_ru);
        return;
    }
    const int first = pendingFirst();
    const int last = juce::jmin (divider, lines.size());
    if (last <= first)
    {
        say ("Нет строк для маркера: передвиньте разделительную строку вниз"_ru);
        return;
    }

    if (! fits (first, last))
    {
        say ("Блок не помещается на экран телефона: уменьшите число строк"_ru);
        return;
    }

    const double bs = beatSeconds();
    const double beat = clock / bs;
    const double defaultEnd = juce::jmax (engine.getSongEnd() / bs, beat + 16.0);
    const int index = data.sections.add (first, last, beat, defaultEnd);
    if (index < 0)
    {
        say ("Здесь уже стоит маркер: выберите другое место на таймлайне"_ru);
        return;
    }
    selected = index;
    resetDivider();
    say ("Маркер "_ru + juce::String (index + 1) + ": строки "_ru + juce::String (first + 1) + "-" + juce::String (last));
    sectionsChanged();
}

void LyricsPanel::deleteSelected()
{
    if (selected < 0 || selected >= data.sections.size())
        return;
    data.sections.remove (selected);
    selected = -1;
    resetDivider();
    sectionsChanged();
}

void LyricsPanel::showDeleteMenu (int index)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Удалить маркер "_ru + juce::String (index + 1));
    juce::Component::SafePointer<LyricsPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options(), [safe, index] (int result)
    {
        if (safe != nullptr && result == 1)
        {
            safe->select (index);
            safe->deleteSelected();
        }
    });
}

void LyricsPanel::sectionsChanged()
{
    dirty = true;
    dirtyAt = juce::Time::getMillisecondCounter();
    publish();
    overlay->repaint();
    timeline->repaint();
}

// ---- files

void LyricsPanel::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Текст песни"_ru, juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   "*.txt;*.lrc");
    juce::Component::SafePointer<LyricsPanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || ! fc.getResult().existsAsFile())
                                  return;
                              juce::MemoryBlock block;
                              if (fc.getResult().loadFileAsData (block))
                                  safe->loadText (LyricsData::decode (block));
                          });
}

void LyricsPanel::loadText (const juce::String& rawText)
{
    auto text = rawText.replace ("\r\n", "\n").replace ("\r", "\n");
    juce::String plain;
    LyricsSections timed;
    const bool lrc = LyricsData::parseLrc (text, beatSeconds(), plain, timed);

    auto apply = [this, text, plain, timed, lrc]
    {
        data.sections = lrc ? timed : LyricsSections();
        editor.setText (lrc ? plain : text, false);
        refreshLines();
        data.text = editor.getText();
        selected = -1;
        count = juce::jmin (count, juce::jmax (1, lines.size()));
        resetDivider();
        sectionsChanged();
        say (lrc ? "Загружен текст с метками времени: маркеров "_ru + juce::String (data.sections.size())
                 : "Текст загружен: "_ru + juce::String (lines.size()) + " строк"_ru);
    };

    if (data.sections.empty())
    {
        apply();
        return;
    }
    juce::Component::SafePointer<LyricsPanel> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "TC",
                                        "Заменить текст? Маркеры этой песни будут удалены."_ru, {}, {}, this,
                                        juce::ModalCallbackFunction::create ([safe, apply] (int result)
                                        {
                                            if (safe != nullptr && result == 1)
                                                apply();
                                        }));
}

void LyricsPanel::setSong (const juce::String& name, const juce::File& file)
{
    if (file == songFile && name == title)
        return;

    saveNow();
    songFile = file;
    title = name;
    data.load (file);
    editor.setText (data.text, false);
    refreshLines();
    selected = -1;
    count = juce::jmax (1, (int) countSlider.getValue());
    resetDivider();
    timeline->viewStart = 0.0;
    publish();
    overlay->repaint();
    timeline->repaint();
}

void LyricsPanel::setPlaylist (const juce::String& current, const juce::String& next, const juce::String& listJson)
{
    server.publishPlaylist (current, next, listJson);
}

void LyricsPanel::saveNow()
{
    if (dirty)
        data.save (songFile);
    dirty = false;
}

void LyricsPanel::publish()
{
    if (serverOk)
        server.publishText (title, wrapped, data.sections.items);
}

void LyricsPanel::timerCallback()
{
    const juce::uint32 now = juce::Time::getMillisecondCounter();
    if (dirty && now - dirtyAt > 500)
    {
        saveNow();
        publish();   // the text itself changed while typing
    }

    const double clock = engine.getClockSeconds();
    const double bs = beatSeconds();
    const double beat = clock / bs;
    const int section = clock >= 0.0 ? data.sections.sectionAt (beat) : -1;
    const int finished = clock >= 0.0 ? data.sections.numFinished (beat) : 0;
    if (serverOk)
        server.publishLive (clock, engine.isPlaying() && ! engine.isPaused(), engine.isPlaying() && engine.isPaused(), section, finished, engine.getBpm(), engine.getTranspose());

    bool overlayDirty = false;
    if (section != playingSection)
    {
        playingSection = section;
        overlayDirty = true;
    }

    updateGeometry();
    // The text was scrolled / resized since the last tick. Compared with the previous tick, not with the last paint:
    // scrolling repaints the editor area (and the overlay there) but not the gutter with the braces.
    if (geo.top0 != paintedGeo.top0 || geo.pitch != paintedGeo.pitch)
    {
        overlayDirty = true;
        paintedGeo = geo;
    }
    if (overlayDirty)
        overlay->repaint();

    if (clock != shownClock)
    {
        shownClock = clock;
        timeLabel.setText (formatTime (clock), juce::dontSendNotification);
        if (engine.isPlaying())
            timeline->followPlayhead (clock);
        timeline->repaint();
    }
    playButton.setActive (engine.isPlaying() && ! engine.isPaused());

    // status line
    if (now < statusUntil)
    {
        hintLabel.setText (statusText, juce::dontSendNotification);
        hintLabel.setColour (juce::Label::textColourId, Pal::accent);
    }
    else
    {
        hintLabel.setText ("Тяните разделительную строку по тексту, нажмите + на нужном моменте песни (F9). "
                           "Скобки у текста и на таймлайне можно двигать, стыки двигаются вместе. Ctrl + колесо - масштаб таймлайна."_ru,
                           juce::dontSendNotification);
        hintLabel.setColour (juce::Label::textColourId, Pal::textDim);
    }

    // info about the block
    juce::String info;
    if (selected >= 0 && selected < data.sections.size())
    {
        const auto& s = data.sections.items[(size_t) selected];
        info = "Маркер "_ru + juce::String (selected + 1) + ": строки "_ru + juce::String (s.first + 1) + "-" + juce::String (s.last)
             + ", "_ru + formatTime (s.t0 * bs) + " - " + formatTime (s.t1 * bs)
             + (fits (s.first, s.last) ? juce::String() : ". Не помещается на экран телефона!"_ru);
    }
    else
    {
        const int pf = pendingFirst();
        info = divider > pf ? "Следующий блок: строки "_ru + juce::String (pf + 1) + "-" + juce::String (divider)
                                + ", экран телефона занят на "_ru + juce::String (juce::roundToInt (100.0f * (blockHeight (pf, divider)
                                    + (divider < lines.size() ? phoneScreen::lineGap + phoneScreen::rowHeight : 0.0f)) / phoneScreen::areaHeight)) + "%"
                            : "Все строки разложены или разделительная строка стоит наверху"_ru;
    }
    if (infoLabel.getText() != info)
        infoLabel.setText (info, juce::dontSendNotification);
}

// ---------------------------------------------------------------- window

LyricsWindow::LyricsWindow (Engine& e)
    : juce::DocumentWindow ("Текст песни"_ru, Pal::surface, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
{
    setUsingNativeTitleBar (true);
    panel = new LyricsPanel (e);
    panel->setSize (980, 780);
    setContentOwned (panel, true);
    setResizable (true, true);
    setResizeLimits (640, 520, 2200, 1800);
    centreWithSize (980, 780);
}
