#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif
#include "MainComponent.h"
#include "TempoDetect.h"
#include "Builtin.h"

// ---------------------------------------------------------------- plugin window

// content of a plugin window: a thin bar with the on / off switch above the editor of the plugin
class PluginFrame : public juce::Component, private juce::ComponentListener, private juce::Timer
{
public:
    static constexpr int barHeight = 34;

    PluginFrame (juce::AudioProcessorEditor* editorToOwn, std::function<bool()> isBypassed, std::function<void()> toggleBypass)
        : editor (editorToOwn), bypassed (std::move (isBypassed)), toggle (std::move (toggleBypass)),
          power (IconButton::Icon::Power)
    {
        addAndMakeVisible (editor.get());
        editor->addComponentListener (this);
        power.setTooltip ("Включить / выключить плагин"_ru);
        power.onClick = [this] { if (toggle) toggle(); refreshState(); };
        addAndMakeVisible (power);
        refreshState();
        setSize (juce::jmax (260, editor->getWidth()), editor->getHeight() + barHeight);
        startTimerHz (8);
    }

    ~PluginFrame() override { editor->removeComponentListener (this); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Pal::surface);
        g.setColour (on ? Pal::text : Pal::textDim);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (on ? "Включён"_ru : "Выключен (обход)"_ru, 52, 0, getWidth() - 60, barHeight, juce::Justification::centredLeft);
    }

    void resized() override
    {
        power.setBounds (10, 4, 34, barHeight - 8);
        editor->setTopLeftPosition (0, barHeight);
        if (editor->getWidth() != getWidth() || editor->getHeight() != getHeight() - barHeight)
            editor->setSize (getWidth(), getHeight() - barHeight);
    }

private:
    void componentMovedOrResized (juce::Component& c, bool, bool wasResized) override
    {
        if (&c == editor.get() && wasResized && (getWidth() != editor->getWidth() || getHeight() != editor->getHeight() + barHeight))
            setSize (juce::jmax (260, editor->getWidth()), editor->getHeight() + barHeight);
    }

    void timerCallback() override { refreshState(); }
    void refreshState()
    {
        const bool now = bypassed ? ! bypassed() : true;
        if (now != on)
        {
            on = now;
            repaint();
        }
        power.setActive (on);
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::function<bool()> bypassed;
    std::function<void()> toggle;
    IconButton power;
    bool on = true;
};

PluginWindow::PluginWindow (juce::AudioPluginInstance& pluginToShow, std::function<void (PluginWindow*)> onCloseRequested,
                            std::function<bool()> isBypassed, std::function<void()> toggleBypass)
    : juce::DocumentWindow (pluginToShow.getName(), Pal::surface, juce::DocumentWindow::closeButton),
      plugin (&pluginToShow),
      onClose (std::move (onCloseRequested))
{
    setUsingNativeTitleBar (true);

    // The built-in effects always get their own editor (never the plain list of parameters, which was shown
    // when the second open found no editor); a plugin whose old editor is still alive gives it back
    juce::PluginDescription description;
    plugin->fillInPluginDescription (description);
    const bool builtIn = description.pluginFormatName == "TC";

    juce::AudioProcessorEditor* editor = nullptr;
    if (builtIn)
    {
        editor = plugin->createEditor();
    }
    else if (plugin->hasEditor())
        editor = plugin->createEditorIfNeeded();

    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor (*plugin);

    const bool resizableEditor = editor->isResizable();
    setContentOwned (new PluginFrame (editor, std::move (isBypassed), std::move (toggleBypass)), true);
    setResizable (resizableEditor, false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

// ---------------------------------------------------------------- plugin chooser (tick the plugins you want)

namespace
{
    class PluginChooserComponent : public juce::Component,
                                   private juce::ChangeListener
    {
    public:
        PluginChooserComponent (Engine& e, std::function<void()> scanRequested)
            : engine (e), onScan (std::move (scanRequested))
        {
            hint.setText ("Отметьте плагины, которые нужны в меню слотов дорожек."_ru, juce::dontSendNotification);
            hint.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            addAndMakeVisible (hint);

            scanButton.setButtonText ("Сканировать VST3..."_ru);
            scanButton.onClick = [this] { if (onScan) onScan(); };
            addAndMakeVisible (scanButton);

            allButton.setButtonText ("Выбрать все"_ru);
            allButton.onClick = [this] { engine.setAllPluginsEnabled (true); rebuild(); };
            addAndMakeVisible (allButton);

            noneButton.setButtonText ("Снять все"_ru);
            noneButton.onClick = [this] { engine.setAllPluginsEnabled (false); rebuild(); };
            addAndMakeVisible (noneButton);

            viewport.setViewedComponent (&content, false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);

            engine.knownPlugins.addChangeListener (this);
            rebuild();
        }

        ~PluginChooserComponent() override { engine.knownPlugins.removeChangeListener (this); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (10);
            auto top = r.removeFromTop (30);
            scanButton.setBounds (top.removeFromRight (170));
            top.removeFromRight (6);
            noneButton.setBounds (top.removeFromRight (110));
            top.removeFromRight (6);
            allButton.setBounds (top.removeFromRight (120));
            hint.setBounds (top);
            r.removeFromTop (6);
            viewport.setBounds (r);
            layoutRows();
        }

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override { rebuild(); }

        void rebuild()
        {
            rows.clear();
            descriptions.clear();

            std::vector<juce::PluginDescription> list;
            for (auto& d : engine.knownPlugins.getTypes())
                if (! d.isInstrument)
                    list.push_back (d);
            std::sort (list.begin(), list.end(), [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
            {
                return a.name.compareIgnoreCase (b.name) < 0;
            });

            for (auto& d : list)
            {
                auto* b = rows.add (new juce::ToggleButton (d.name + "   (" + d.manufacturerName + ")"));
                b->setToggleState (engine.isPluginEnabled (d), juce::dontSendNotification);
                const auto copy = d;
                b->onClick = [this, b, copy] { engine.setPluginEnabled (copy, b->getToggleState()); };
                content.addAndMakeVisible (b);
            }

            hint.setText (rows.isEmpty()
                              ? "Список пуст. Нажмите \"Сканировать VST3\"."_ru
                              : "Отметьте плагины, которые нужны в меню слотов дорожек."_ru,
                          juce::dontSendNotification);
            layoutRows();
        }

        void layoutRows()
        {
            const int w = juce::jmax (100, viewport.getMaximumVisibleWidth());
            int y = 0;
            for (auto* b : rows)
            {
                b->setBounds (0, y, w, 26);
                y += 28;
            }
            content.setSize (w, juce::jmax (y, 10));
        }

        Engine& engine;
        std::function<void()> onScan;
        juce::Label hint;
        juce::TextButton scanButton, allButton, noneButton;
        juce::Viewport viewport;
        juce::Component content;
        juce::OwnedArray<juce::ToggleButton> rows;
        std::vector<juce::PluginDescription> descriptions;
    };

    void launchDialog (juce::Component* content, const juce::String& title, bool resizable)
    {
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (content);
        o.dialogTitle = title;
        o.dialogBackgroundColour = Pal::surface;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = resizable;
        o.launchAsync();
    }

    juce::StringArray activeChannelNames (juce::AudioIODevice* device, bool input)
    {
        juce::StringArray result;
        if (device == nullptr)
            return result;

        const auto all = input ? device->getInputChannelNames() : device->getOutputChannelNames();
        const auto active = input ? device->getActiveInputChannels() : device->getActiveOutputChannels();
        for (int i = 0; i < all.size(); ++i)
            if (active[i])
                result.add (all[i]);
        return result;
    }
}

// ---------------------------------------------------------------- main component

MainComponent::MainComponent()
    : vblank (this, [this] { updateTimeline(); })
{
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    juce::PropertiesFile::Options opts;
    opts.applicationName = "TC";
    opts.filenameSuffix = ".settings";
    opts.folderName = "TC";
    opts.storageFormat = juce::PropertiesFile::storeAsXML;
    pluginScanProps = std::make_unique<juce::PropertiesFile> (opts);

    engine.initialise();
    engine.onTracksChanged = [this] { rebuildTracks(); };
    engine.deviceManager.addChangeListener (this);
    engine.knownPlugins.addChangeListener (this);

    menuButton.setTooltip ("Меню: драйвер, плагины, настройки"_ru);
    menuButton.onClick = [this] { showMainMenu(); };
    addAndMakeVisible (menuButton);

    bpmSlider.setValue (engine.getBpm(), juce::dontSendNotification);
    bpmSlider.onValueChange = [this] { engine.setTempo (bpmSlider.getValue()); };
    addAndMakeVisible (bpmSlider);

    tapButton.onClick = [this] { tapTempo(); };
    addAndMakeVisible (tapButton);

    recordButton.setTooltip ("Запись: пишет входы всех дорожек с нажатой кнопкой REC, пока кнопка включена. "_ru
                             "Папка - в главном меню."_ru);
    recordButton.onClick = [this] { toggleRecording(); };
    addAndMakeVisible (recordButton);
    recTimeLabel.setJustificationType (juce::Justification::centredLeft);
    recTimeLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    recTimeLabel.setColour (juce::Label::textColourId, Pal::red);
    recTimeLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (recTimeLabel);

    keyLabel.setButtonText ("Тон: -"_ru);
    keyLabel.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    keyLabel.setColour (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    keyLabel.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    keyLabel.setTooltip ("Тональность микса. Клик: сдвиг до нужной тональности, сброс"_ru);
    keyLabel.onClick = [this] { showKeyMenu(); };
    addAndMakeVisible (keyLabel);

    keyDown.setTooltip ("Тональность всего микса на полтона ниже (без изменения темпа)"_ru);
    keyDown.onClick = [this] { engine.setTranspose (engine.getTranspose() - 1); updateKeyDisplay(); };
    addAndMakeVisible (keyDown);
    keyUp.setTooltip ("Тональность всего микса на полтона выше (без изменения темпа)"_ru);
    keyUp.onClick = [this] { engine.setTranspose (engine.getTranspose() + 1); updateKeyDisplay(); };
    addAndMakeVisible (keyUp);

    analysisButton.setTooltip ("Определение темпа и тональности: вручную, по миксу или Live (входной канал)"_ru);
    analysisButton.onClick = [this] { showAnalysisSettings(); };
    addAndMakeVisible (analysisButton);
    engine.onAnalysisDone = [safe = juce::Component::SafePointer<MainComponent> (this)] { if (safe != nullptr) { safe->updateKeyDisplay(); safe->refreshStrips(); safe->repaintTimeline(); } };

    playAllButton.setTooltip ("Играть. Если метроном включён хотя бы на одной дорожке - сначала 4 удара отсчёта (такт перед первым)."_ru);
    playAllButton.onClick = [this]
    {
        if (! engine.isAudioAlive())
        {
            say ("Аудиоустройство не запущено - выберите драйвер и устройство вывода"_ru
                 + (engine.audioInitError.isNotEmpty() ? " (" + engine.audioInitError + ")" : juce::String()));
            showAudioSettings();
            return;
        }
        engine.playAll();
    };
    addAndMakeVisible (playAllButton);
    pauseAllButton.setTooltip ("Пауза: всё замирает, Play продолжит с этого места"_ru);
    pauseAllButton.onClick = [this] { engine.pauseAll(); };
    addAndMakeVisible (pauseAllButton);
    stopAllButton.setTooltip ("Стоп: остановить всё и вернуться в начало"_ru);
    stopAllButton.onClick = [this] { engine.stopAll(); };
    addAndMakeVisible (stopAllButton);

    cpuLabel.setJustificationType (juce::Justification::centredRight);
    cpuLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    cpuLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (cpuLabel);

    addTrackButton.setTooltip ("Добавить дорожку"_ru);
    addTrackButton.onClick = [this] { addTrack(); };
    addTrackButton.setColour (juce::TextButton::buttonColourId, Pal::surfaceLo);
    trackList.addAndMakeVisible (addTrackButton);
    trackList.addChildComponent (trackDropLine);
    trackList.onMove = [this] (const juce::String& d, juce::Point<int> p) { trackDragMove (d, p); };
    trackList.onDrop = [this] (const juce::String& d, juce::Point<int> p) { trackDropped (d, p); };
    trackList.onLeave = [this] { trackDropLine.setVisible (false); };

    viewport.setViewedComponent (&trackList, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    ruler.onSeek = [this] (double t) { seekTimeline (t); };
    ruler.onZoom = [this] (float d) { zoomTimeline (d); };
    addAndMakeVisible (ruler);
    scroller.onSeek = [this] (double t) { seekTimeline (t); };
    addAndMakeVisible (scroller);
    addAndMakeVisible (timeReadout);
    addAndMakeVisible (playhead);

    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    statusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (statusLabel);

    emptyHint.setText ("Добавьте дорожку, затем перетащите на неё аудиофайл (или правая кнопка по дорожке)."_ru, juce::dontSendNotification);
    emptyHint.setJustificationType (juce::Justification::centred);
    emptyHint.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (emptyHint);

    setSize (1280, 720);

    addAndMakeVisible (playerBar);
    playerBar.cb.heightChanged = [this] { resized(); repaint(); };
    playerBar.cb.save = [this] (PlayerBar::Tab tab) { saveSelectionToFile (tab); };
    playerBar.cb.lyrics = [this] { showLyrics(); };
    playerBar.cb.previous = [this] { stepSong (-1); };
    playerBar.cb.next = [this] { stepSong (1); };
    playerBar.cb.rowMoved = [this] (PlayerBar::Tab tab, int from, int to) { onPlayerRowMoved (tab, from, to); };
    playerBar.cb.rowClick = [this] (PlayerBar::Tab tab, int id) { onPlayerRowClicked (tab, id); };
    playerBar.cb.rowDouble = [this] (PlayerBar::Tab tab, int id)
    {
        onPlayerRowClicked (tab, id);
        if (tab == PlayerBar::Tab::Seq) renameProject();
        else if (tab == PlayerBar::Tab::Playlist) renamePlaylist();
        else renameSequence();
    };
    playerBar.cb.add = [this] (PlayerBar::Tab tab)
    {
        auto warn = [] (const juce::String& text)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "TC", text);
        };
        if (tab != PlayerBar::Tab::Seq && library.projectNames().isEmpty())
        {
            warn ("Плейлист и трек нельзя создать без секвенции. Сначала создайте секвенцию (вкладка Seq)."_ru);
            return;
        }
        if (tab == PlayerBar::Tab::Tracks && playlist.name.isEmpty())
        {
            warn ("Трек нельзя создать без плейлиста. Сначала создайте плейлист (вкладка Playlist)."_ru);
            return;
        }
        if (tab == PlayerBar::Tab::Seq)
            askText ("Новая секвенция"_ru, "Название (например, группа):"_ru, "Новая секвенция"_ru,
                     [this] (juce::String n) { createProject (n); });
        else if (tab == PlayerBar::Tab::Playlist)
            askText ("Новый плейлист"_ru, "Название:"_ru, "Плейлист"_ru, [this] (juce::String n) { createPlaylist (n); });
        else
            askText ("Новый трек"_ru, "Название:"_ru, "Трек"_ru, [this] (juce::String n) { createSequence (n, true); });
    };
    playerBar.cb.remove = [this] (PlayerBar::Tab tab)
    {
        deleteSelectedRows (tab);
    };

    initialiseLibrary();
    bpmSlider.setValue (engine.getBpm(), juce::dontSendNotification);
    rebuildTracks();
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    engine.deviceManager.removeChangeListener (this);
    engine.knownPlugins.removeChangeListener (this);
    engine.onTracksChanged = nullptr;
    engine.onAnalysisDone = nullptr;
    lyricsWindow.reset();
    pluginWindows.clear();
    trackComponents.clear();
    engine.shutdown();
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Pal::window);
    const int topH = 62 + playerBar.getPreferredHeight();
    g.setColour (Pal::surfaceLo);
    g.fillRect (0, 0, getWidth(), 62);
    g.setColour (Pal::hairline);
    g.fillRect (0, 62, getWidth(), 1);
    g.fillRect (0, topH, getWidth(), 1);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    auto rowA = area.removeFromTop (62).reduced (12, 9);

    menuButton.setBounds (rowA.removeFromLeft (48));
    cpuLabel.setBounds (rowA.removeFromRight (110));

    // tempo, key, gear and the big transport buttons, centred in the whole window
    const int groupW = 170 + 8 + 56 + 8 + 44 + 4 + 62 + 12 + 34 + 2 + 124 + 2 + 34 + 8 + 44 + 30 + 72 + 6 + 64 + 6 + 64;
    auto group = juce::Rectangle<int> (groupW, rowA.getHeight()).withCentre ({ getWidth() / 2, rowA.getCentreY() });
    const auto groupBox = group;
    bpmSlider.setBounds (group.removeFromLeft (170));
    group.removeFromLeft (8);
    tapButton.setBounds (group.removeFromLeft (56));
    group.removeFromLeft (8);
    recordButton.setBounds (group.removeFromLeft (44));
    group.removeFromLeft (4);
    recTimeLabel.setBounds (group.removeFromLeft (62));
    group.removeFromLeft (20);
    keyDown.setBounds (group.removeFromLeft (34));
    group.removeFromLeft (2);
    keyLabel.setBounds (group.removeFromLeft (124));
    group.removeFromLeft (2);
    keyUp.setBounds (group.removeFromLeft (34));
    group.removeFromLeft (8);
    analysisButton.setBounds (group.removeFromLeft (44));
    group.removeFromLeft (30);
    playAllButton.setBounds (group.removeFromLeft (72));
    group.removeFromLeft (6);
    pauseAllButton.setBounds (group.removeFromLeft (64));
    group.removeFromLeft (6);
    stopAllButton.setBounds (group.removeFromLeft (64));

    // elapsed / remaining time: free space between the transport and the CPU readout
    {
        const int x = groupBox.getRight() + 24;
        const int room = cpuLabel.getX() - 12 - x;
        timeReadout.setVisible (room >= 150);
        timeReadout.setBounds (x, rowA.getY(), juce::jmin (room, 230), rowA.getHeight());
    }

    playerBar.setBounds (area.removeFromTop (playerBar.getPreferredHeight()));

    area.removeFromTop (28 + scrollerH);   // scroller + ruler rows (placed in layoutTimelineParts)
    viewport.setBounds (area.reduced (6, 0).withTrimmedBottom (6));
    emptyHint.setBounds (area.reduced (40, 20).withTrimmedLeft (TrackComponent::headerWidth));
    layoutTracks();
}

void MainComponent::layoutTimelineParts()
{
    const int rulerH = 28;
    const int visibleW = viewport.getMaximumVisibleWidth();
    const int laneX = viewport.getX() + TrackComponent::headerWidth;
    const int laneW = juce::jmax (100, visibleW - TrackComponent::headerWidth);
    const int top = viewport.getY() - rulerH;

    statusLabel.setBounds (viewport.getX() + 10, top, TrackComponent::headerWidth - 20, rulerH);
    ruler.setBounds (laneX, top, laneW, rulerH);
    scroller.setBounds (laneX, top - scrollerH, laneW, scrollerH);
    playhead.setBounds (laneX, top, laneW, rulerH + viewport.getHeight());
    timelineDirty = true;
}

void MainComponent::layoutTracks()
{
    // the vertical scrollbar appears once the rows are taller than the viewport and takes some width:
    // rows, ruler and playhead must all use the final width, or the grid drifts between ruler and lanes
    int w = juce::jmax (TrackComponent::headerWidth + 100, viewport.getMaximumVisibleWidth());
    for (int pass = 0; pass < 3; ++pass)
    {
        int y = 0;
        for (auto* t : trackComponents)
        {
            t->setBounds (0, y, w, TrackComponent::rowHeight);
            y += TrackComponent::rowHeight;
        }

        addTrackButton.setBounds (8, y + 8, TrackComponent::headerWidth - 20, 34);
        y += 50;
        trackList.setSize (w, juce::jmax (y, viewport.getHeight()));

        const int settled = juce::jmax (TrackComponent::headerWidth + 100, viewport.getMaximumVisibleWidth());
        if (settled == w)
            break;
        w = settled;
    }
    emptyHint.setVisible (trackComponents.isEmpty());
    layoutTimelineParts();
}

// ---------------------------------------------------------------- tracks

void MainComponent::rebuildTracks()
{
    trackComponents.clear();

    TrackComponent::Callbacks cb;
    cb.remove = [this] (std::shared_ptr<Track> t)
    {
        for (int i = 0; i < Track::numSlots; ++i)
            if (auto* s = t->getSlot (i))
                closeEditorsFor (s->plugin.get());

        // deleting a component from its own button callback is unsafe, so defer
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe, t]
        {
            if (safe != nullptr)
                safe->engine.removeTrack (t);
        });
    };
    cb.inputMenu = [this] (std::shared_ptr<Track> t, juce::Component* c) { showInputMenu (t, c); };
    cb.slotClick = [this] (std::shared_ptr<Track> t, int i, juce::Component* c)
    {
        if (t->getSlot (i) == nullptr)
            showSlotMenu (t, i, c);            // empty slot: choose a plugin
        else
            openPluginEditor (t, i);
    };
    cb.slotBypass = [this] (std::shared_ptr<Track> t, int i)
    {
        if (auto* s = t->getSlot (i))
        {
            s->bypass = ! s->bypass.load();
            t->refreshLatency();
            refreshStrips();
        }
    };
    cb.slotMenu = [this] (std::shared_ptr<Track> t, int i, juce::Component* c) { showSlotMenu (t, i, c); };
    cb.outputMenu = [this] (std::shared_ptr<Track> t, juce::Component* c) { showOutputMenu (t, c); };
    cb.slotDropped = [this] (std::shared_ptr<Track> t, int i, const juce::String& payload, bool copy) { slotDropped (t, i, payload, copy); };

    LaneComponent::Callbacks lane;
    lane.seek = [this] (double t) { seekTimeline (t); };
    lane.zoom = [this] (float d) { zoomTimeline (d); };
    lane.clipPressed = [this] (std::shared_ptr<Track> t, std::shared_ptr<Clip> c, const juce::ModifierKeys& m) { clipPressed (t, c, m); };
    lane.clipClickedAlone = [this] (std::shared_ptr<Clip> c) { timelineView.selected = { c->id }; repaintTimeline(); };
    lane.dragSelected = [this] (std::shared_ptr<Clip> c, double delta) { dragSelectedClips (c, delta); };
    lane.emptyPressed = [this] (const juce::ModifierKeys& m) { emptyPressed (m); };
    lane.marquee = [this] (juce::Rectangle<int> r, bool finished) { marqueeSelect (r, finished); };
    lane.focus = [this] { grabKeyboardFocus(); };
    lane.clipMenu = [this] (std::shared_ptr<Track> t, std::shared_ptr<Clip> c, juce::Point<int> p) { showClipMenu (t, c, p); raiseMenus(); };
    lane.laneMenu = [this] (std::shared_ptr<Track> t, double time, juce::Point<int> p) { showLaneMenu (t, time, p); raiseMenus(); };
    lane.filesDropped = [this] (std::shared_ptr<Track> t, const juce::StringArray& f, double time) { addAudioFiles (t, f, time); };
    lane.changed = [this] { repaintTimeline(); };

    for (auto& t : engine.getTracks())
    {
        auto* tc = new TrackComponent (engine, t, timelineView, cb, lane);
        trackComponents.add (tc);
        trackList.addAndMakeVisible (tc);
    }
    layoutTracks();
    timelineDirty = true;
}

int MainComponent::trackRowForDrag (const juce::String& description, juce::Point<int> pos, int& from) const
{
    const int id = description.fromFirstOccurrenceOf ("tctrack:", false, false).getIntValue();
    const auto tracksNow = engine.getTracks();
    const int n = (int) tracksNow.size();
    from = -1;
    for (int i = 0; i < n; ++i)
        if (tracksNow[(size_t) i]->id == id)
            from = i;
    return juce::jlimit (0, juce::jmax (0, n - 1), pos.y / TrackComponent::rowHeight);
}

void MainComponent::trackDragMove (const juce::String& description, juce::Point<int> pos)
{
    int from = -1;
    const int to = trackRowForDrag (description, pos, from);
    if (from < 0)
        return;

    const auto inViewport = viewport.getLocalPoint (&trackList, pos);
    viewport.autoScroll (viewport.getWidth() / 2, inViewport.y, 50, 16);

    trackDropLine.setBounds (0, (to + (to > from ? 1 : 0)) * TrackComponent::rowHeight - 2, trackList.getWidth(), 4);
    trackDropLine.setVisible (to != from);
    trackDropLine.toFront (false);
}

void MainComponent::trackDropped (const juce::String& description, juce::Point<int> pos)
{
    int from = -1;
    const int to = trackRowForDrag (description, pos, from);
    trackDropLine.setVisible (false);
    if (from < 0 || from == to)
        return;
    const int id = description.fromFirstOccurrenceOf ("tctrack:", false, false).getIntValue();
    auto t = engine.findTrack (id);
    if (t == nullptr)
        return;
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::MessageManager::callAsync ([safe, t, to]
    {
        if (safe != nullptr)
            safe->engine.moveTrack (t, to);
    });
}

void MainComponent::refreshStrips()
{
    for (auto* t : trackComponents)
        t->refreshAll();
}

void MainComponent::addTrack()
{
    const int count = (int) engine.getTracks().size();
    auto track = engine.addTrack ("Дорожка "_ru + juce::String (count + 1));
    track->inputL = -1;      // new tracks have no input until one is chosen
    track->inputR = -1;
    refreshStrips();
}

// ---------------------------------------------------------------- timeline

void MainComponent::repaintTimeline()
{
    ruler.repaint();
    scroller.repaint();
    playhead.repaint();
    for (auto* t : trackComponents)
        t->repaintLane();
}

void MainComponent::zoomTimeline (float wheelDelta)
{
    if (wheelDelta == 0.0f)
        return;
    timelineView.span = juce::jlimit (2.0, 600.0, timelineView.span * (wheelDelta > 0.0f ? 0.8 : 1.25));   // wheel up = zoom in
    repaintTimeline();
}

void MainComponent::seekTimeline (double clockSeconds)
{
    engine.seek (clockSeconds);
    timelineView.clock = engine.getClockSeconds();
    if (engine.isPlaying())
        timelineView.clock = clockSeconds;
    repaintTimeline();
}

void MainComponent::chooseAudioFor (const std::shared_ptr<Track>& track, double time)
{
    chooser = std::make_unique<juce::FileChooser> ("Выберите аудиофайлы"_ru, juce::File(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [safe, track, time] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr)
                                  return;
                              juce::StringArray files;
                              for (auto& f : fc.getResults())
                                  files.add (f.getFullPathName());
                              if (! files.isEmpty())
                                  safe->addAudioFiles (track, files, time);
                          });
}

void MainComponent::addAudioFiles (const std::shared_ptr<Track>& track, const juce::StringArray& files, double time)
{
    juce::Component::SafePointer<MainComponent> safe (this);
    auto sorted = files;
    sorted.sort (true);

    // A multitrack is one song: stems whose own tempo is unclear (vocals, pads) take the tempo of the surest stem.
    struct Batch
    {
        std::vector<std::shared_ptr<Clip>> clips;
        int pending = 0;
    };
    auto batch = std::make_shared<Batch>();
    batch->pending = sorted.size();

    auto place = [this, safe, time, batch] (const std::shared_ptr<Track>& target, const juce::String& path)
    {
        engine.addClipFromFile (target, juce::File (path), time,
                                [safe, path, batch] (std::shared_ptr<Clip> clip, const juce::String& error)
                                {
                                    if (safe == nullptr)
                                        return;
                                    if (clip == nullptr)
                                    {
                                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "TC",
                                                                                error + ":\n"_ru + path);
                                        --batch->pending;   // nothing to analyse
                                    }
                                    safe->refreshStrips();
                                    safe->repaintTimeline();
                                },
                                [safe, batch] (std::shared_ptr<Clip> clip)
                                {
                                    if (safe == nullptr)
                                        return;
                                    batch->clips.push_back (clip);

                                    if (--batch->pending == 0 && batch->clips.size() > 1)
                                    {
                                        std::shared_ptr<Clip> best;
                                        for (auto& c : batch->clips)
                                            if (c->original != nullptr && c->original->fileBpm > 0.0
                                                && (best == nullptr || c->original->tempoConfidence > best->original->tempoConfidence))
                                                best = c;
                                        if (best != nullptr && best->original->tempoConfidence >= 0.6)
                                            for (auto& c : batch->clips)
                                                if (c->original != nullptr && c->original->fileBpm <= 0.0)
                                                {
                                                    c->original->fileBpm = best->original->fileBpm;
                                                    if (c->data != nullptr)
                                                        c->data->fileBpm = best->original->fileBpm;
                                                    if (auto owner = safe->trackOf (c))
                                                        safe->engine.ensureLive (owner, c);
                                                }
                                    }
                                    safe->refreshStrips();
                                    safe->repaintTimeline();
                                });
    };

    for (int i = 0; i < sorted.size(); ++i)
    {
        const juce::File f (sorted[i]);
        std::shared_ptr<Track> target = track;

        // every file of a multitrack gets its own track; an existing lane is used only while it is still empty
        if (target == nullptr || ! target->getClips().empty() || i > 0)
        {
            target = engine.addTrack (f.getFileNameWithoutExtension());   // one new track per extra file
            target->inputL = -1;
            target->inputR = -1;
        }
        else if (target->name.startsWith ("Дорожка"_ru) && target->getClips().empty())
            target->name = f.getFileNameWithoutExtension();

        place (target, sorted[i]);
    }
    refreshStrips();
    repaintTimeline();
}

bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (LaneComponent::isAudioFile (f))
            return true;
    return false;
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    juce::StringArray audio;
    for (auto& f : files)
        if (LaneComponent::isAudioFile (f))
            audio.add (f);
    if (! audio.isEmpty())
        addAudioFiles (nullptr, audio, 0.0);   // dropped outside any lane: a new track per file, all on bar 1
}

void MainComponent::showLaneMenu (const std::shared_ptr<Track>& track, double time, juce::Point<int> screenPos)
{
    juce::PopupMenu m;
    m.addItem (1, "Добавить аудио сюда..."_ru);
    m.addSeparator();
    m.addItem (2, "Привязка к сетке"_ru, true, timelineView.snap);
    m.addItem (3, "Не менять тональность этой дорожки (ударные)"_ru, true, ! track->transposeOn.load());

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                     [safe, track, time] (int r)
                     {
                         if (safe == nullptr)
                             return;
                         if (r == 1)
                             safe->chooseAudioFor (track, time);
                         else if (r == 2)
                             safe->timelineView.snap = ! safe->timelineView.snap;
                         else if (r == 3)
                             track->transposeOn.store (! track->transposeOn.load());
                     });
}

std::shared_ptr<Track> MainComponent::trackOf (const std::shared_ptr<Clip>& clip) const
{
    for (auto& t : engine.getTracks())
        for (auto& c : t->getClips())
            if (c == clip)
                return t;
    return nullptr;
}

// Re-stretch the clips that follow the project tempo (called once the tempo has stopped changing).
void MainComponent::showClipMenu (const std::shared_ptr<Track>& track, const std::shared_ptr<Clip>& clip, juce::Point<int> screenPos)
{
    if (timelineView.selected.count (clip->id) == 0)
        timelineView.selected = { clip->id };   // right click on a clip outside the selection selects just it
    repaintTimeline();

    const auto group = selectedClips();
    const bool many = group.size() > 1;
    const auto source = clip->original != nullptr ? clip->original : clip->data;

    bool tempoKnown = true;
    for (auto& c : group)
        tempoKnown = tempoKnown && c->original != nullptr && c->original->fileBpm > 0.0;

    // tempo line: the file's own tempo, and the one it was stretched to
    juce::String tempoText;
    if (many)
        tempoText = "Выбрано клипов: "_ru + juce::String ((int) group.size());
    else if (source->fileBpm <= 0.0)
        tempoText = "Темп: не определён"_ru;
    else
    {
        tempoText = "Темп: "_ru + juce::String (source->fileBpm, 2) + " BPM";
        const auto arrow = "  " + juce::String::fromUTF8 ("\xe2\x86\x92") + "  ";
        if (clip->follows())
            tempoText += arrow + juce::String (engine.getBpm(), 2) + " BPM";   // follows the project tempo live
        else if (clip->stretchedToBpm.load() > 0.0)
            tempoText += arrow + juce::String (clip->stretchedToBpm.load(), 2) + " BPM";
        if (clip->stretchTarget.load() > 0.0)
            tempoText += "  (растягиваю...)"_ru;
    }

    juce::PopupMenu m;
    m.addSectionHeader (many ? "Несколько клипов"_ru : source->file.getFileName());
    m.addItem (100, tempoText, false);
    if (! many && source->keyMass > 1.0f)
    {
        float h[12];
        for (int i = 0; i < 12; ++i)
            h[i] = source->keyHist[i];
        const KeyGuess kg = guessKey (h);
        m.addItem (101, kg.corr >= 0.5 ? "Тональность: "_ru + keyNameFull (kg.tonic, kg.minor)
                                       : "Тональность: не выражена (ударные или шум)"_ru, false);
    }
    m.addSeparator();
    m.addItem (1, "Открыть в проводнике"_ru, ! many);
    m.addItem (2, "Заменить..."_ru, ! many);
    m.addItem (3, "Поставить на 1 такт"_ru);
    m.addItem (4, "Привязка к сетке"_ru, true, timelineView.snap);
    m.addItem (5, "Привязка к темпу"_ru, tempoKnown, clip->followTempo.load());
    bool anyFollow = false;
    for (auto& c : group)
        anyFollow = anyFollow || c->followTempo.load();
    m.addItem (6, "Растянуть по темпу"_ru, tempoKnown && ! anyFollow);   // a following clip is already at the project tempo
    m.addSeparator();
    m.addItem (7, many ? "Удалить выбранные"_ru : "Удалить"_ru);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                     [safe, track, clip, group, source] (int r)
                     {
                         if (safe == nullptr || r <= 0)
                             return;

                         auto& eng = safe->engine;
                         switch (r)
                         {
                             case 1: source->file.revealToUser(); break;
                             case 2:
                             {
                                 safe->chooser = std::make_unique<juce::FileChooser> ("Выберите аудиофайл"_ru, juce::File(),
                                                                                      "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
                                 safe->chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                                             [safe, track, clip] (const juce::FileChooser& fc)
                                                             {
                                                                 if (safe == nullptr || fc.getResult() == juce::File())
                                                                     return;
                                                                 const double start = clip->start.load();
                                                                 const bool loop = clip->loop.load();
                                                                 const bool follow = clip->followTempo.load();
                                                                 const double stretched = clip->stretchedToBpm.load();
                                                                 safe->engine.addClipFromFile (track, fc.getResult(), start,
                                                                     [safe, track, clip, loop, follow, stretched] (std::shared_ptr<Clip> fresh, const juce::String&)
                                                                     {
                                                                         if (safe == nullptr || fresh == nullptr)
                                                                             return;
                                                                         fresh->loop = loop;
                                                                         fresh->followTempo = follow;
                                                                         fresh->updateSpeed (safe->engine.getBpm());
                                                                         safe->engine.removeClip (track, clip->id);
                                                                         safe->timelineView.selected.clear();
                                                                         safe->repaintTimeline();
                                                                     },
                                                                     [safe, track, follow, stretched] (std::shared_ptr<Clip> fresh)
                                                                     {
                                                                         // the tempo of the new file is known now
                                                                         if (safe != nullptr && stretched > 0.0 && ! follow)
                                                                             safe->engine.stretchClip (track, fresh, stretched,
                                                                                                       [safe] { if (safe != nullptr) safe->repaintTimeline(); });
                                                                     });
                                                             });
                                 break;
                             }
                             case 3: for (auto& c : group) c->start.store (0.0); eng.clipsChanged(); break;
                             case 4: safe->timelineView.snap = ! safe->timelineView.snap; break;
                             case 5:
                             {
                                 const bool newValue = ! clip->followTempo.load();
                                 for (auto& c : group)
                                 {
                                     c->followTempo = newValue;
                                     if (auto owner = safe->trackOf (c))
                                     {
                                         if (newValue)
                                         {
                                             eng.ensureLive (owner, c);   // the real-time stretcher for this clip
                                             if (c->stretchedToBpm.load() > 0.0 && c->original != nullptr)
                                                 eng.stretchClip (owner, c, c->original->fileBpm);   // back to the untouched file
                                         }
                                     }
                                 }
                                 eng.refreshClipSpeeds();
                                 eng.clipsChanged();
                                 break;
                             }
                             case 6:
                                 for (auto& c : group)
                                     if (auto owner = safe->trackOf (c))
                                         eng.stretchClip (owner, c, eng.getBpm(), [safe] { if (safe != nullptr) safe->repaintTimeline(); });
                                 break;
                             case 7: safe->deleteSelectedClips(); break;
                             default: break;
                         }
                         safe->repaintTimeline();
                     });
}

// ---------------------------------------------------------------- strip menus

void MainComponent::showInputMenu (const std::shared_ptr<Track>& track, juce::Component* anchor)
{
    juce::PopupMenu menu, bus;

    bool anyBus = false;
    for (auto& t : engine.getTracks())
    {
        if (t == track || engine.wouldCreateCycle (*track, t->id))
            continue;
        bus.addItem (10000 + t->id, t->name, true, track->busId.load() == t->id);
        anyBus = true;
    }
    if (! anyBus)
        bus.addItem (9999, "(нет других дорожек)"_ru, false);

    menu.addSubMenu ("Bus", bus);
    menu.addSeparator();

    const int il = track->inputL.load();
    const int ir = track->inputR.load();
    const bool noBus = track->busId.load() < 0;
    menu.addItem (1, "Нет"_ru, true, noBus && il < 0);

    const auto ins = activeChannelNames (engine.deviceManager.getCurrentAudioDevice(), true);
    for (int i = 0; i < ins.size(); ++i)
        menu.addItem (100 + i, "Вход "_ru + juce::String (i + 1) + ": " + ins[i], true,
                      noBus && il == i && (ir < 0 || ir == i));

    if (ins.size() >= 2)
        menu.addSeparator();
    for (int i = 0; i + 1 < ins.size(); i += 2)
        menu.addItem (500 + i, "Вход "_ru + juce::String (i + 1) + "+" + juce::String (i + 2) + " (стерео)"_ru, true,
                      noBus && il == i && ir == i + 1);

    juce::Component::SafePointer<MainComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor),
                        [safe, track] (int result)
                        {
                            if (safe == nullptr || result <= 0 || result == 9999)
                                return;

                            track->inputChosenByUser = true;
                            if (result >= 10000)
                            {
                                track->busId = result - 10000;
                            }
                            else if (result == 1)
                            {
                                track->busId = -1;
                                track->inputL = -1;
                                track->inputR = -1;
                            }
                            else if (result >= 500)
                            {
                                track->busId = -1;
                                track->inputL = result - 500;
                                track->inputR = result - 500 + 1;
                            }
                            else if (result >= 100)
                            {
                                track->busId = -1;
                                track->inputL = result - 100;
                                track->inputR = -1;
                            }
                            safe->refreshStrips();
                        });
}

void MainComponent::showOutputMenu (const std::shared_ptr<Track>& track, juce::Component* anchor)
{
    juce::PopupMenu menu;
    const int current = track->outputPair.load();
    menu.addItem (1, "Нет"_ru, true, current < 0);
    menu.addSeparator();

    const auto outs = activeChannelNames (engine.deviceManager.getCurrentAudioDevice(), false);
    const int pairs = juce::jmax (1, (outs.size() + 1) / 2);
    for (int p = 0; p < pairs; ++p)
    {
        juce::String label = "Выход "_ru + juce::String (p * 2 + 1) + "-" + juce::String (p * 2 + 2);
        if (p * 2 < outs.size())
            label += ": " + outs[p * 2];
        if (p * 2 + 1 < outs.size())
            label += " / " + outs[p * 2 + 1];
        menu.addItem (10 + p, label, true, current == p);
    }

    juce::Component::SafePointer<MainComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor),
                        [safe, track] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;
                            track->outputPair = result == 1 ? -1 : result - 10;
                            safe->refreshStrips();
                        });
}

void MainComponent::showSlotMenu (const std::shared_ptr<Track>& track, int slotIndex, juce::Component* anchor)
{
    auto* slot = track->getSlot (slotIndex);
    const auto effects = engine.getEnabledEffects();

    auto addPluginItems = [&effects] (juce::PopupMenu& m)
    {
        if (effects.empty())
        {
            m.addItem (9999, "Нет отмеченных плагинов (Меню > Список плагинов)"_ru, false);
            return;
        }
        for (size_t i = 0; i < effects.size(); ++i)
            m.addItem (1000 + (int) i, effects[i].name + "   (" + effects[i].manufacturerName + ")");
    };

    juce::PopupMenu menu;
    if (slot != nullptr)
    {
        menu.addItem (1, "Открыть окно плагина"_ru);
        menu.addItem (3, "Bypass", true, slot->bypass.load());
        menu.addItem (4, "Убрать из слота"_ru);
        menu.addSeparator();

        juce::PopupMenu replace;
        addPluginItems (replace);
        menu.addSubMenu ("Заменить на"_ru, replace);
    }
    else
    {
        addPluginItems (menu);
    }

    juce::Component::SafePointer<MainComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor),
                        [safe, track, slotIndex, effects] (int result)
                        {
                            if (safe == nullptr || result <= 0 || result == 9999)
                                return;

                            auto closeSlotEditor = [&]
                            {
                                if (auto* s = track->getSlot (slotIndex))
                                    safe->closeEditorsFor (s->plugin.get());
                            };

                            if (result == 1)
                            {
                                safe->openPluginEditor (track, slotIndex);
                            }
                            else if (result == 3)
                            {
                                if (auto* s = track->getSlot (slotIndex))
                                {
                                    s->bypass = ! s->bypass.load();
                                    track->refreshLatency();
                                    safe->refreshStrips();
                                }
                            }
                            else if (result == 4)
                            {
                                closeSlotEditor();
                                track->clearSlot (slotIndex);
                                safe->refreshStrips();
                            }
                            else if (result >= 1000 && result - 1000 < (int) effects.size())
                            {
                                closeSlotEditor();
                                track->clearSlot (slotIndex);
                                safe->loadPluginIntoSlot (track, slotIndex, effects[(size_t) (result - 1000)]);
                                safe->refreshStrips();
                            }
                        });
}

void MainComponent::loadPluginIntoSlot (const std::shared_ptr<Track>& track, int slotIndex,
                                        const juce::PluginDescription& desc)
{
    juce::Component::SafePointer<MainComponent> safe (this);
    engine.addPluginToTrack (track, slotIndex, desc, [safe, desc] (bool ok, const juce::String& error)
    {
        if (safe == nullptr)
            return;
        if (! ok)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "TC",
                                                    "Не удалось загрузить плагин \""_ru + desc.name + "\":\n"_ru + error);
        safe->refreshStrips();
    });
}

void MainComponent::slotDropped (const std::shared_ptr<Track>& target, int targetIndex, const juce::String& payload, bool copy)
{
    // payload: "tcslot:<track id>:<slot index>"
    const auto parts = juce::StringArray::fromTokens (payload, ":", "");
    if (parts.size() != 3 || target == nullptr)
        return;

    const int sourceId = parts[1].getIntValue(), sourceIndex = parts[2].getIntValue();
    std::shared_ptr<Track> source;
    for (auto& t : engine.getTracks())
        if (t->id == sourceId)
            source = t;

    auto* sourceSlot = source != nullptr ? source->getSlot (sourceIndex) : nullptr;
    if (sourceSlot == nullptr || sourceSlot->plugin == nullptr || (source == target && sourceIndex == targetIndex))
        return;

    if (copy)
    {
        // Ctrl: a new instance of the same plugin with the same settings; a busy slot is not overwritten
        int index = targetIndex;
        if (target->getSlot (index) != nullptr)
            index = target->findFreeSlot();
        if (index < 0)
        {
            say ("Нет свободного слота для копии плагина"_ru);
            return;
        }

        juce::MemoryBlock state;
        sourceSlot->plugin->getStateInformation (state);
        const auto description = sourceSlot->description;
        juce::Component::SafePointer<MainComponent> safe (this);
        engine.addPluginToTrack (target, index, description,
                                 [safe, description] (bool ok, const juce::String& error)
                                 {
                                     if (safe == nullptr)
                                         return;
                                     if (! ok)
                                         juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "TC",
                                                                                 "Не удалось скопировать плагин \""_ru + description.name + "\":\n"_ru + error);
                                     safe->refreshStrips();
                                 },
                                 juce::Base64::toBase64 (state.getData(), state.getSize()), sourceSlot->bypass.load());
        return;
    }

    if (source == target)
        source->moveSlot (sourceIndex, targetIndex);
    else
        source->swapSlotWith (sourceIndex, *target, targetIndex);   // an occupied target slot swaps places
    refreshStrips();
}

void MainComponent::openPluginEditor (const std::shared_ptr<Track>& track, int slotIndex)
{
    auto* slot = track->getSlot (slotIndex);
    if (slot == nullptr || slot->plugin == nullptr)
        return;

    for (auto* w : pluginWindows)
        if (w->getPlugin() == slot->plugin.get())
        {
            w->toFront (true);
            return;
        }

    juce::Component::SafePointer<MainComponent> safe (this);
    auto* instance = slot->plugin.get();
    std::weak_ptr<Track> weakTrack = track;
    auto findSlot = [weakTrack, instance]() -> Track::Slot*
    {
        if (auto t = weakTrack.lock())
            for (int i = 0; i < Track::numSlots; ++i)
                if (auto* sl = t->getSlot (i))
                    if (sl->plugin.get() == instance)
                        return sl;
        return nullptr;
    };
    auto isBypassed = [findSlot] { auto* sl = findSlot(); return sl != nullptr && sl->bypass.load(); };
    auto toggle = [safe, weakTrack, findSlot]
    {
        if (auto* sl = findSlot())
        {
            sl->bypass = ! sl->bypass.load();
            if (auto t = weakTrack.lock())
                t->refreshLatency();
            if (safe != nullptr)
                safe->refreshStrips();
        }
    };
    pluginWindows.add (new PluginWindow (*slot->plugin, [safe] (PluginWindow* w)
    {
        juce::MessageManager::callAsync ([safe, w]
        {
            if (safe != nullptr)
                safe->pluginWindows.removeObject (w, true);
        });
    }, isBypassed, toggle));
}

void MainComponent::closeEditorsFor (juce::AudioPluginInstance* plugin)
{
    if (plugin == nullptr)
        return;
    for (int i = pluginWindows.size(); --i >= 0;)
        if (pluginWindows[i]->getPlugin() == plugin)
            pluginWindows.remove (i);
}

// ---------------------------------------------------------------- top bar / dialogs

void MainComponent::showMainMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Настройки аудио (драйвер, устройство, буфер)..."_ru);
    menu.addSeparator();
    menu.addItem (2, "Плагины: отметить нужные..."_ru);
    menu.addItem (3, "Плагины: сканировать VST3..."_ru);
    menu.addSeparator();
    menu.addItem (4, "Добавить дорожку"_ru);
    menu.addItem (7, "Привязка клипов к сетке"_ru, true, timelineView.snap);
    menu.addItem (6, "Сохранить всё"_ru);
    menu.addSeparator();
    menu.addItem (10, "Запись: папка - "_ru + engine.getRecordFolder().getFullPathName() + "  (изменить...)"_ru);
    menu.addItem (11, "Сохранить секвенцию..."_ru);
    menu.addItem (12, "Импорт..."_ru);

    juce::Component::SafePointer<MainComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton),
                        [safe] (int result)
                        {
                            if (safe == nullptr)
                                return;
                            switch (result)
                            {
                                case 1: safe->showAudioSettings(); break;
                                case 2: safe->showPluginChooser(); break;
                                case 3: safe->showPluginScanner(); break;
                                case 4: safe->addTrack(); break;
                                case 6: safe->saveEverything (true); break;
                                case 10: safe->chooseRecordFolder(); break;
                                case 11: safe->exportSequence(); break;
                                case 12: safe->importFromFile(); break;
                                case 7: safe->timelineView.snap = ! safe->timelineView.snap; break;
                                default: break;
                            }
                        });
}

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (engine.deviceManager, 0, 64, 0, 64,
                                                             false, false, false, false);
    selector->setSize (560, 520);
    launchDialog (selector, "Аудио (драйвер, устройство, каналы, буфер)"_ru, false);
}

void MainComponent::showPluginChooser()
{
    juce::Component::SafePointer<MainComponent> safe (this);
    auto* c = new PluginChooserComponent (engine, [safe] { if (safe != nullptr) safe->showPluginScanner(); });
    c->setSize (560, 480);
    launchDialog (c, "Плагины для слотов дорожек"_ru, true);
}

void MainComponent::showPluginScanner()
{
    auto* list = new juce::PluginListComponent (engine.pluginFormats, engine.knownPlugins,
                                                engine.getDataDirectory().getChildFile ("scan-deadman.txt"),
                                                pluginScanProps.get(), true);
    list->setSize (720, 460);
    launchDialog (list, "Сканирование плагинов VST3"_ru, true);
}

void MainComponent::showAnalysisSettings()
{
    const int km = engine.keyMode.load(), tm = engine.tempoMode.load();
    const int curTrack = engine.liveTrackId.load();

    juce::PopupMenu trackMenu;
    juce::String trackText = "первая дорожка со входом"_ru;
    trackMenu.addItem (1000, "Первая дорожка со входом (авто)"_ru, true, curTrack < 0);
    int idx = 0;
    for (auto& t : engine.getTracks())
    {
        const bool hasIn = t->inputL.load() >= 0;
        trackMenu.addItem (1001 + idx, t->name + (hasIn ? juce::String() : "  (нет входа: задайте на дорожке)"_ru), true, curTrack == t->id);
        if (curTrack == t->id)
            trackText = t->name;
        ++idx;
    }

    juce::PopupMenu m;
    m.addSectionHeader ("Тональность"_ru);
    m.addItem (10, "Вручную (без определения)"_ru, true, km == 0);
    m.addItem (11, "Определять по миксу"_ru, true, km == 1);
    m.addItem (12, "Определять Live (по входу дорожки)"_ru, true, km == 2);
    m.addSectionHeader ("Темп"_ru);
    m.addItem (20, "Вручную"_ru, true, tm == 0);
    m.addItem (21, "Определять по миксу"_ru, true, tm == 1);
    m.addItem (22, "Определять Live (по входу дорожки)"_ru, true, tm == 2);
    m.addSectionHeader ("Live слушает вход дорожки"_ru);
    m.addSubMenu ("Дорожка: "_ru + trackText, trackMenu);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&analysisButton),
                     [safe] (int r)
                     {
                         if (safe == nullptr || r <= 0)
                             return;
                         auto& eng = safe->engine;
                         if (r >= 10 && r <= 12)
                         {
                             eng.keyMode = r - 10;
                         }
                         else if (r >= 20 && r <= 22)
                         {
                             eng.tempoMode = r - 20;
                             safe->mixSignature = -1;   // the mix tempo is applied once right away
                         }
                         else if (r == 3)
                             eng.liveKey.reset();
                         else if (r >= 1000)
                         {
                             const auto& tr = eng.getTracks();
                             eng.liveTrackId = r == 1000 ? -1 : (r - 1001 < (int) tr.size() ? tr[(size_t) (r - 1001)]->id : -1);
                             eng.liveKey.reset();
                         }

                         eng.applyAnalysisModes();
                         eng.saveLiveKeySettings();
                         safe->updateKeyDisplay();
                         safe->raiseMenus();

                     });
    raiseMenus();
}

// Called twice a second: tempo from the mix / from the input, when those modes are on.
void MainComponent::updateAutoTempo()
{
    engine.syncLiveSource();
    const int tm = engine.tempoMode.load();
    if (tm == 1)
    {
        // a new signature = the set of clips (or their detected tempos) changed
        long long sig = 0;
        for (auto& t : engine.getTracks())
            for (auto& c : t->getClips())
                if (c->original != nullptr)
                    sig = sig * 31 + (long long) c->id + (long long) std::llround (c->original->fileBpm * 100.0);
        if (sig != mixSignature)
        {
            mixSignature = sig;
            const double bpm = engine.getMixTempo();
            if (bpm > 0.0 && std::abs (bpm - engine.getBpm()) > 0.05)
            {
                engine.setTempo (bpm);
                bpmSlider.setValue (bpm, juce::dontSendNotification);
            }
        }
    }
    else if (tm == 2)
    {
        const auto live = engine.liveKey.getState();
        if (live.active && live.bpm > 0.0 && live.bpmStamp != liveBpmStamp)
        {
            liveBpmStamp = live.bpmStamp;
            // the same value twice in a row: no jumping on a single doubtful reading
            if (std::abs (live.bpm - liveBpmCandidate) < 0.8)
                ++liveBpmAgree;
            else
                liveBpmAgree = 1;
            liveBpmCandidate = live.bpm;

            if (liveBpmAgree >= 2 && std::abs (live.bpm - engine.getBpm()) > 0.4 && ! bpmSlider.isMouseButtonDown())
            {
                const double bpm = std::round (live.bpm * 10.0) / 10.0;
                engine.setTempo (bpm);
                bpmSlider.setValue (bpm, juce::dontSendNotification);
            }
        }
    }
}

// Text of the key button: the key of the live input if one is listened to, otherwise the key of the mix.
void MainComponent::updateKeyDisplay()
{
    engine.syncLiveSource();
    juce::String text = "Тон: -"_ru, tip;
    const int mode = engine.keyMode.load();
    const int shift = engine.getTranspose();

    if (mode == 2)
    {
        const auto live = engine.liveKey.getState();
        if (live.active && live.valid)
        {
            text = "Вход: "_ru + keyName (live.key.tonic, live.key.minor);
            tip = "Тональность по входному каналу: "_ru + keyNameFull (live.key.tonic, live.key.minor)
                  + ", по последним "_ru + juce::String ((int) live.seconds) + " с"_ru;
        }
        else
        {
            text = live.active ? "Вход: ..."_ru : "Вход: -"_ru;
            tip = live.active ? "Слушаю входной канал, нужно несколько секунд музыки"_ru
                              : "Выберите входной канал в настройках (шестерёнка)"_ru;
        }
    }
    else if (mode == 1)
    {
        int used = 0;
        const KeyGuess proj = engine.getProjectKey (&used);
        if (proj.tonic >= 0)
        {
            const int t = ((proj.tonic + shift) % 12 + 12) % 12;
            text = "Тон: "_ru + keyName (t, proj.minor);
            if (shift != 0)
                text += juce::String (shift > 0 ? " +" : " ") + juce::String (shift);
            tip = "Тональность микса по нейросети: "_ru + keyNameFull (proj.tonic, proj.minor)
                  + (shift != 0 ? ", сдвиг "_ru + juce::String (shift) + " -> " + keyNameFull (t, proj.minor) : juce::String())
                  + ", по клипам: "_ru + juce::String (used);
            for (auto& tr : engine.getTracks())
                for (auto& cl : tr->getClips())
                {
                    const ClipData* d = cl->original.get();
                    if (d == nullptr || d->keyMass <= 1.0f)
                        continue;
                    const KeyGuess kg = guessKey (d->keyHist);
                    tip += "\n"_ru + (tr->muted.load() ? "(выкл) "_ru : juce::String()) + d->file.getFileName() + ": "
                           + (kg.corr >= 0.5 ? keyName (kg.tonic, kg.minor) : "нет тональности"_ru);
                }
        }
        else
            tip = "Тональность ещё не определена: нужны клипы с выраженными нотами"_ru;
    }
    else
        tip = "Тональность задаётся вручную (шестерёнка: определение по миксу или Live)"_ru;

    if (mode != 2 && shift != 0 && text == "Тон: -"_ru)
        text = "Тон: "_ru + juce::String (shift > 0 ? "+" : "") + juce::String (shift);

    if (mode != 0 && keyEngineError().isNotEmpty())
        tip += "\nНейросеть недоступна: "_ru + keyEngineError();

    if (keyLabel.getButtonText() != text)
        keyLabel.setButtonText (text);
    keyLabel.setTooltip (tip + "\nКлик: сдвиг до нужной тональности, сброс"_ru);
}

void MainComponent::showKeyMenu()
{
    int used = 0;
    const KeyGuess proj = engine.getProjectKey (&used);
    const int shift = engine.getTranspose();

    juce::PopupMenu m;
    if (proj.tonic >= 0)
        m.addItem (200, "Тональность микса: "_ru + keyNameFull (proj.tonic, proj.minor), false);
    else
        m.addItem (200, "Тональность ещё не определена"_ru, false);
    m.addItem (1, "Сбросить сдвиг (0)"_ru, shift != 0);
    if (proj.tonic >= 0)
    {
        juce::PopupMenu to;
        for (int t = 0; t < 12; ++t)
            to.addItem (300 + t, keyNameFull (t, proj.minor), true, ((proj.tonic + shift) % 12 + 12) % 12 == t);
        m.addSubMenu ("Перейти в тональность"_ru, to);
    }

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&keyLabel),
                     [safe, proj] (int r)
                     {
                         if (safe == nullptr || r <= 0)
                             return;
                         if (r == 1)
                             safe->engine.setTranspose (0);
                         else if (r >= 300 && r < 312)
                         {
                             int d = (r - 300 - proj.tonic + 12) % 12;   // 0..11 semitones up
                             if (d > 6)
                                 d -= 12;                                  // the nearer way
                             safe->engine.setTranspose (d);
                         }
                         safe->updateKeyDisplay();
                         safe->raiseMenus();
                     });
    raiseMenus();
}

void MainComponent::setPinned (bool shouldPin)
{
    pinned = shouldPin;
    if (auto* top = getTopLevelComponent())
        top->setAlwaysOnTop (pinned);
}

void MainComponent::tapTempo()
{
    const auto now = juce::Time::getMillisecondCounter();
    if (! tapTimes.isEmpty() && now - tapTimes.getLast() > 2000)
        tapTimes.clear();

    tapTimes.add (now);
    while (tapTimes.size() > 8)
        tapTimes.remove (0);

    if (tapTimes.size() >= 2)
    {
        const double avg = (double) (tapTimes.getLast() - tapTimes.getFirst()) / (double) (tapTimes.size() - 1);
        if (avg > 0.0)
        {
            const double tapped = juce::jlimit (20.0, 400.0, 60000.0 / avg);
            engine.setTempo (tapped);
            bpmSlider.setValue (tapped, juce::dontSendNotification);
        }
    }
}

// ---------------------------------------------------------------- timers / listeners

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &engine.knownPlugins)
    {
        // the built-in effects cannot be removed from the list ("Remove" / "Clear list" in the scanner dialog)
        for (auto& d : BuiltinFormat::allDescriptions())
            engine.knownPlugins.addType (d);
        engine.savePluginList();
    }
}

// Runs once per screen refresh. The audio clock only advances once per audio block, so the displayed position
// is extrapolated with the wall clock and gently pulled towards the real one: smooth, never jumps back.
void MainComponent::updateTimeline()
{
    if (! isDragAndDropActive())
    {
        // a strip drag ended (dropped or cancelled): bring the strips back to normal
        for (auto* t : trackComponents)
            t->endDragVisual();
        trackDropLine.setVisible (false);
    }
    {
        const bool rec = engine.isRecording();
        if (rec)
        {
            const double t = engine.getRecordedSeconds();
            recordButton.setSpin ((float) std::fmod (t * 5.0, 6.2831853));
            const int s = (int) t;
            recTimeLabel.setText (juce::String::formatted ("%02d:%02d", s / 60, s % 60), juce::dontSendNotification);
        }
        else if (recTimeLabel.getText().isNotEmpty())
        {
            recTimeLabel.setText ({}, juce::dontSendNotification);
            recordButton.setActive (false);
        }
    }

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double engineClock = engine.getClockSeconds();
    if (engineClock != engineClockSeen)
    {
        engineClockSeen = engineClock;
        engineClockStamp = now;
    }

    const double frameDt = juce::jlimit (0.0, 0.1, now - lastFrameStamp);   // first frame: lastFrameStamp is 0
    if (engine.isPlaying() && ! engine.isPaused())
    {
        // the clock stamp must be fresh: right after Play the audio thread has not advanced the clock yet,
        // and a stale stamp (0 = system start) would extrapolate the position by the whole uptime
        const double predicted = engineClock + juce::jlimit (0.0, 0.1, now - engineClockStamp);
        if (std::abs (predicted - displayClock) > 0.25)
            displayClock = predicted;
        else
            displayClock += frameDt + (predicted - displayClock - frameDt) * 0.1;
    }
    else
    {
        displayClock = engineClock;
        engineClockSeen = engineClock;
        engineClockStamp = now;   // keep the stamp fresh while stopped / paused
    }

    lastFrameStamp = now;
    timelineView.clock = displayClock;
    timeReadout.setTimes (juce::jmax (0.0, displayClock), juce::jmax (0.0, engine.getSongEnd() - displayClock));

    const float laneW = (float) juce::jmax (1, ruler.getWidth());
    const double scroll = timelineView.scrollPx (laneW);
    if (timelineDirty || scroll != lastScroll || timelineView.span != lastSpan || engine.getBpm() != lastBpm)
    {
        lastScroll = scroll;
        lastSpan = timelineView.span;
        lastBpm = engine.getBpm();
        timelineDirty = false;
        repaintTimeline();
    }
}

// Menus popping up and being dismissed by a click elsewhere can make Windows drop the "topmost" state of the
// window, which then sinks behind other programs. Check the real state and restore it.
// A menu opened by the click that dismissed another menu can end up under the window Windows raises at that
// moment. Bring every other (popup) window of the program in front of the main one shortly afterwards.
void MainComponent::raiseMenus()
{
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Timer::callAfterDelay (40, [safe]
    {
        if (safe == nullptr)
            return;
        auto* top = safe->getTopLevelComponent();
        auto& desktop = juce::Desktop::getInstance();
        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* c = desktop.getComponent (i))
                if (c != top && c->isVisible() && c->isAlwaysOnTop())
                    c->toFront (false);
    });
}

void MainComponent::keepOnTop()
{
    if (! pinned)
        return;
    auto* top = getTopLevelComponent();
    if (top == nullptr)
        return;
    auto* peer = top->getPeer();
    if (peer == nullptr)
        return;

   #if JUCE_WINDOWS
    const auto hwnd = (HWND) peer->getNativeHandle();
    const bool topmost = (GetWindowLongPtr (hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (! topmost)
    {
        engine.logLine ("window lost topmost: restoring");
        SetWindowPos (hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
   #else
    if (! top->isAlwaysOnTop())
        top->setAlwaysOnTop (true);
   #endif
}

void MainComponent::timerCallback()
{
    keepOnTop();
    syncLyrics();
    for (auto* t : trackComponents)
        t->updateLive();

    playAllButton.setActive (engine.isPlaying());
    pauseAllButton.setActive (engine.isPaused());
    if (engine.consumeTempoApplied())
    {
        engine.clipsChanged();   // clip positions moved with the tempo
        repaintTimeline();
    }

    if (std::abs (bpmSlider.getValue() - engine.getBpm()) > 0.001 && ! bpmSlider.isMouseButtonDown())
        bpmSlider.setValue (engine.getBpm(), juce::dontSendNotification);

    if (++slowCounter >= 15)   // twice a second
    {
        slowCounter = 0;
        updateKeyDisplay();
        updateAutoTempo();
        {   // diagnostics: engine state to %APPDATA%\TC\log.txt while playing
            static juce::int64 lastLogged = -1;
            const auto cb = engine.getCallbackCount();
            static int idleTicks = 0;
            if (engine.isPlaying() || (++idleTicks % 10) == 1)
            {
                lastLogged = cb;
                engine.logLine ("tick disp=" + juce::String (displayClock) + " main=" + getBounds().toString() + " top=" + (getTopLevelComponent() != nullptr ? getTopLevelComponent()->getBounds().toString() : juce::String()) + " | " + engine.describeState());
            }
        }
        cpuLabel.setText ("CPU " + juce::String ((int) std::round (engine.getCpuLoad() * 100.0f)) + "%",
                          juce::dontSendNotification);
        cpuLabel.setTooltip (juce::String (engine.getSampleRate(), 0) + " Гц, блок "_ru
                             + juce::String (engine.getBlockSize()) + "; " + engine.describeDeviceLatency());
    }

    if (++autosaveCounter >= 30 * 30)   // every 30 s
    {
        autosaveCounter = 0;
        engine.saveActiveSequence();
    }
}


// ---------------------------------------------------------------- sequences and playlists

void MainComponent::say (const juce::String& message)
{
    statusLabel.setText (message, juce::dontSendNotification);
}


void MainComponent::askText (const juce::String& title, const juce::String& label, const juce::String& initial,
                             std::function<void (juce::String)> done)
{
    auto* w = new juce::AlertWindow (title, label, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("text", initial);
    w->addButton ("Готово"_ru, 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Отмена"_ru, 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true,
                        juce::ModalCallbackFunction::create ([w, done] (int result)
                        {
                            if (result == 1 && done)
                            {
                                const auto text = w->getTextEditorContents ("text").trim();
                                if (text.isNotEmpty())
                                    done (text);
                            }
                        }),
                        true);
}

void MainComponent::confirmDelete (const juce::String& message, std::function<void()> done)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle ("TC")
                                      .withMessage (message)
                                      .withButton ("Удалить"_ru)
                                      .withButton ("Отмена"_ru),
                                  [done] (int result) { if (result == 1 && done) done(); });
}

void MainComponent::initialiseLibrary()
{
    auto projects = library.projectNames();
    const bool fresh = projects.isEmpty();
    if (fresh)
    {
        library.createProject ("Моя секвенция"_ru);
        projects = library.projectNames();
    }

    juce::String proj, comp, plName;
    library.loadSession (proj, comp, plName);
    if (! projects.contains (proj))
    {
        proj = projects[0];
        comp = {};
        plName = {};
    }
    library.setProject (proj);

    // a project of the first versions: one project.xml -> the first composition
    if (fresh && engine.getLegacyProjectFile().existsAsFile())
        engine.getLegacyProjectFile().copyFileTo (library.sequenceFile ("Трек 1"_ru));

    playlist = Playlist();
    if (plName.isNotEmpty())
        library.loadPlaylist (plName, playlist);

    enterProject (comp);
}

void MainComponent::enterProject (juce::String preferredComposition)
{
    const auto comps = library.sequenceNames();
    juce::String target = preferredComposition;
    if (target.isEmpty() || ! comps.contains (target))
        target = ! playlist.items.isEmpty() && comps.contains (playlist.items[0]) ? playlist.items[0]
                 : comps.isEmpty() ? juce::String ("Трек 1"_ru) : comps[0];

    switchToSequence (target, playlist.items.indexOf (target));
    if (comps.isEmpty())
        engine.saveActiveSequence();   // creates the file
}

void MainComponent::switchProject (juce::String name)
{
    saveEverything (false);
    library.setProject (name);
    playlist = Playlist();
    playlistIndex = -1;

    const auto lists = library.playlistNames();
    if (! lists.isEmpty())
        library.loadPlaylist (lists[0], playlist);

    enterProject ({});
}

void MainComponent::createProject (const juce::String& wanted)
{
    const auto name = Library::uniqueName (library.projectNames(), Library::sanitize (wanted));
    saveEverything (false);
    library.createProject (name);
    switchProject (name);
}

void MainComponent::renameProject()
{
    askText ("Переименовать секвенцию"_ru, "Новое название:"_ru, library.getProject(),
             [this] (juce::String wanted)
             {
                 const auto oldName = library.getProject();
                 auto names = library.projectNames();
                 names.removeString (oldName);
                 const auto newName = Library::uniqueName (names, Library::sanitize (wanted));
                 if (newName == oldName)
                     return;

                 saveEverything (false);
                 const auto oldDir = library.projectDir (oldName), newDir = library.projectDir (newName);
                 if (! library.renameProject (oldName, newName))
                     return;

                 engine.relocateSequences (oldDir, newDir);
                 refreshPlayerBar();
                 if (auto s = engine.getActiveSequence())
                     library.saveSession (s->name, playlist.name);
             });
}

// The delete button: removes the selected rows of the current tab from the program, no questions asked.
// Audio files on the disk are never touched.
void MainComponent::deleteSelectedRows (PlayerBar::Tab tab)
{
    auto ids = playerBar.getSelectedIds();

    if (tab == PlayerBar::Tab::Seq)
    {
        const auto all = library.projectNames();
        juce::StringArray doomed;
        for (int id : ids)
            if (id >= 0 && id < projectRows.size())
                doomed.addIfNotAlreadyThere (projectRows[id]);
        if (doomed.isEmpty())
            doomed.add (library.getProject());

        auto remaining = all;
        for (auto& d : doomed)
            remaining.removeString (d);
        if (remaining.isEmpty())
        {
            say ("Последнюю секвенцию удалить нельзя"_ru);
            return;
        }

        if (doomed.contains (library.getProject()))
            switchProject (remaining[0]);
        for (auto& d : doomed)
        {
            engine.discardSequences (library.projectDir (d));
            library.deleteProject (d);
            engine.getDataDirectory().getChildFile ("Media").getChildFile ("seq_" + d).deleteRecursively();
        }
        engine.pruneAnalysisCacheSoon();
        refreshPlayerBar();
        say ("Удалено секвенций: "_ru + juce::String (doomed.size()));
    }
    else if (tab == PlayerBar::Tab::Playlist)
    {
        juce::StringArray doomed;
        for (int id : ids)
            if (id >= 0 && id < playlistRows.size())
                doomed.addIfNotAlreadyThere (playlistRows[id]);
        if (doomed.isEmpty() && playlist.name.isNotEmpty())
            doomed.add (playlist.name);
        if (doomed.isEmpty())
            return;

        for (auto& d : doomed)
            library.deletePlaylist (d);
        if (doomed.contains (playlist.name))
        {
            playlist = Playlist();
            playlistIndex = -1;
            const auto left = library.playlistNames();
            if (! left.isEmpty())
                library.loadPlaylist (left[0], playlist);
            auto seq = engine.getActiveSequence();
            playlistIndex = seq != nullptr ? playlist.items.indexOf (seq->name) : -1;
        }
        refreshPlayerBar();
        if (auto s = engine.getActiveSequence())
            library.saveSession (s->name, playlist.name);
        say ("Удалено плейлистов: "_ru + juce::String (doomed.size()));
    }
    else
    {
        juce::StringArray doomed;
        for (int id : ids)
            if (id >= 0 && id < playlist.items.size())
                doomed.addIfNotAlreadyThere (playlist.items[id]);
        if (doomed.isEmpty() && playlistIndex >= 0)
            doomed.add (playlist.items[playlistIndex]);
        if (doomed.isEmpty())
            return;

        auto seq = engine.getActiveSequence();
        const juce::String cur = seq != nullptr ? seq->name : juce::String();

        // the files go FIRST: refreshPlayerBar() (called when the open song changes) adds every song that
        // still exists but is in no playlist back to the playlist, which made deleted tracks "come back"
        for (auto& d : doomed)
        {
            library.removeFromPlaylists (d);
            playlist.items.removeString (d);
            const auto file = library.sequenceFile (d);
            engine.discardSequences (file);   // also when it is the open one: nothing is saved back
            LyricsData::remove (file);
            file.deleteFile();
        }

        auto others = library.sequenceNames();

        if (doomed.contains (cur) || engine.getActiveSequence() == nullptr)
        {
            // the open song went away: open a neighbour (or an empty one if nothing is left)
            juce::String next = ! playlist.items.isEmpty() && others.contains (playlist.items[0]) ? playlist.items[0]
                                : others.isEmpty() ? juce::String ("Трек 1"_ru) : others[0];
            switchToSequence (next, playlist.items.indexOf (next));
        }
        if (others.isEmpty())
            engine.saveActiveSequence();   // creates the empty replacement
        savePlaylistNow();
        engine.pruneAnalysisCacheSoon();
        auto active = engine.getActiveSequence();
        playlistIndex = active != nullptr ? playlist.items.indexOf (active->name) : -1;
        preloadNeighbours();
        refreshPlayerBar();
        say ("Удалено треков: "_ru + juce::String (doomed.size()));
    }
}

void MainComponent::deleteProject()
{
    const auto cur = library.getProject();
    auto others = library.projectNames();
    others.removeString (cur);
    if (others.isEmpty())
    {
        say ("Последнюю секвенцию удалить нельзя"_ru);
        return;
    }

    confirmDelete ("Удалить секвенцию «"_ru + cur + "» со всеми композициями и плейлистами? Это нельзя отменить."_ru,
                   [this, cur, others]
                   {
                       switchProject (others[0]);
                       library.deleteProject (cur);
                       refreshPlayerBar();
                   });
}

void MainComponent::showLyrics()
{
    if (lyricsWindow == nullptr)
    {
        lyricsWindow = std::make_unique<LyricsWindow> (engine);
        lyricsWindow->getPanel().onTogglePlay = [this] { if (engine.isPlaying()) engine.stopAll(); else engine.playAll(); };
        lyricsWindow->getPanel().onSeek = [this] (double seconds) { seekTimeline (seconds); };
    }
    syncLyrics();
    lyricsWindow->setVisible (true);
    lyricsWindow->toFront (true);
}

void MainComponent::syncLyrics()
{
    if (lyricsWindow == nullptr)
        return;
    auto seq = engine.getActiveSequence();
    lyricsWindow->getPanel().setSong (seq != nullptr ? seq->name : juce::String(), seq != nullptr ? seq->file : juce::File());
}

void MainComponent::refreshPlayerBar()
{
    auto seq = engine.getActiveSequence();
    const juce::String current = seq != nullptr ? seq->name : juce::String();

    // Every track belongs to some playlist: tracks that are in none yet join the current one
    // (a playlist is created if the sequence has none).
    {
        juce::StringArray used = playlist.items;
        for (auto& pn : library.playlistNames())
            if (pn != playlist.name)
            {
                Playlist other;
                if (library.loadPlaylist (pn, other))
                    used.addArray (other.items);
            }

        bool adopted = false;
        for (auto& name : library.sequenceNames())
            if (! used.contains (name))
            {
                if (playlist.name.isEmpty())
                    playlist.name = Library::uniqueName (library.playlistNames(), "Плейлист 1"_ru);
                playlist.items.add (name);
                adopted = true;
            }
        if (adopted)
        {
            savePlaylistNow();
            playlistIndex = seq != nullptr ? playlist.items.indexOf (seq->name) : -1;
            preloadNeighbours();
        }
    }

    const int n = playlist.items.size();

    juce::String next = "-";
    if (playlistIndex >= 0 && playlistIndex + 1 < n)
        next = playlist.items[playlistIndex + 1];

    playerBar.setNow (current, next,
                      playlistIndex >= 0 ? juce::String (playlistIndex + 1) + " / " + juce::String (n) : juce::String());
    playerBar.setCanStep (playlistIndex > 0, playlistIndex >= 0 && playlistIndex + 1 < n);

    std::vector<PlayerBar::Row> seqRows, listRows, trackRows;

    // Seq tab: all sequences
    projectRows = library.projectNames();
    for (int i = 0; i < projectRows.size(); ++i)
        seqRows.push_back ({ projectRows[i], false, projectRows[i] == library.getProject(), i });

    // Playlist tab: the playlists of the current sequence
    playlistRows = library.playlistNames();
    for (int i = 0; i < playlistRows.size(); ++i)
        listRows.push_back ({ playlistRows[i], false, playlistRows[i] == playlist.name, i });

    // Tracks tab: the tracks of the current playlist, then the tracks that are not in it
    for (int i = 0; i < n; ++i)
        trackRows.push_back ({ juce::String (i + 1) + ".  " + playlist.items[i], false, i == playlistIndex, i });

    otherTracks.clear();

    playerBar.setRows (PlayerBar::Tab::Seq, std::move (seqRows));
    playerBar.setRows (PlayerBar::Tab::Playlist, std::move (listRows));
    playerBar.setRows (PlayerBar::Tab::Tracks, std::move (trackRows));
}

void MainComponent::onPlayerRowMoved (PlayerBar::Tab tab, int from, int to)
{
    auto moveIn = [from, to] (juce::StringArray& a)
    {
        if (from < 0 || from >= a.size() || to < 0 || to >= a.size())
            return false;
        const auto s = a[from];
        a.remove (from);
        a.insert (to, s);
        return true;
    };

    if (tab == PlayerBar::Tab::Seq)
    {
        auto a = library.projectNames();
        if (moveIn (a))
            library.setProjectOrder (a);
    }
    else if (tab == PlayerBar::Tab::Playlist)
    {
        auto a = library.playlistNames();
        if (moveIn (a))
            library.setPlaylistOrder (a);
    }
    else if (moveIn (playlist.items))
    {
        auto seq = engine.getActiveSequence();
        playlistIndex = seq != nullptr ? playlist.items.indexOf (seq->name) : -1;
        savePlaylistNow();
    }
    refreshPlayerBar();
}

void MainComponent::onPlayerRowClicked (PlayerBar::Tab tab, int id)
{
    if (tab == PlayerBar::Tab::Seq)
    {
        if (id >= 0 && id < projectRows.size() && projectRows[id] != library.getProject())
            switchProject (projectRows[id]);
    }
    else if (tab == PlayerBar::Tab::Playlist)
    {
        if (id >= 0 && id < playlistRows.size() && playlistRows[id] != playlist.name)
            loadPlaylistByName (playlistRows[id]);
    }
    else
    {
        if (id >= 1000 && id - 1000 < otherTracks.size())
            switchToSequence (otherTracks[id - 1000], -1);
        else if (id >= 0 && id < playlist.items.size())
            switchToSequence (playlist.items[id], id);
    }
}

void MainComponent::loadPlaylistByName (juce::String name)
{
    savePlaylistNow();
    Playlist p;
    if (! library.loadPlaylist (name, p))
        return;
    playlist = p;

    auto seq = engine.getActiveSequence();
    const int at = seq != nullptr ? p.items.indexOf (seq->name) : -1;
    if (at < 0 && ! p.items.isEmpty())
        switchToSequence (p.items[0], 0);
    else
    {
        playlistIndex = at;
        preloadNeighbours();
        refreshPlayerBar();
        library.saveSession (seq != nullptr ? seq->name : juce::String(), p.name);
    }
}

void MainComponent::createPlaylist (const juce::String& wanted)
{
    savePlaylistNow();
    playlist = Playlist();
    playlist.name = Library::uniqueName (library.playlistNames(), Library::sanitize (wanted));
    playlistIndex = -1;
    savePlaylistNow();
    refreshPlayerBar();
    if (auto s = engine.getActiveSequence())
        library.saveSession (s->name, playlist.name);
}

void MainComponent::deletePlaylistConfirm()
{
    if (playlist.name.isEmpty())
        return;

    confirmDelete ("Удалить плейлист «"_ru + playlist.name + "»? Сами треки останутся."_ru,
                   [this]
                   {
                       library.deletePlaylist (playlist.name);
                       playlist = Playlist();
                       playlistIndex = -1;
                       refreshPlayerBar();
                       if (auto s = engine.getActiveSequence())
                           library.saveSession (s->name, {});
                   });
}

void MainComponent::preloadNeighbours()
{
    std::vector<juce::File> keep;
    auto addKeep = [&keep] (const juce::File& f)
    {
        if (std::find (keep.begin(), keep.end(), f) == keep.end())
            keep.push_back (f);
    };

    if (auto seq = engine.getActiveSequence())
    {
        addKeep (seq->file);
        // the songs visited last stay in memory: going back is instant
        recentSongs.erase (std::remove (recentSongs.begin(), recentSongs.end(), seq->file), recentSongs.end());
        recentSongs.insert (recentSongs.begin(), seq->file);
        if (recentSongs.size() > 5)
            recentSongs.resize (5);
    }
    for (auto& f : recentSongs)
        addKeep (f);

    // the next / previous songs of the playlist are loaded in the background, the nearest first
    if (playlistIndex >= 0)
        for (int d = 1; d <= 3; ++d)
            for (int i : { playlistIndex + d, playlistIndex - d })
                if (i >= 0 && i < playlist.items.size())
                {
                    auto f = library.sequenceFile (playlist.items[i]);
                    if (f.existsAsFile())
                    {
                        engine.obtainSequence (f);
                        addKeep (f);
                    }
                }

    engine.dropSequences (keep);
}

void MainComponent::switchToSequence (juce::String name, int indexInPlaylist)
{
    auto seq = engine.obtainSequence (library.sequenceFile (name));
    pluginWindows.clear();
    engine.activateSequence (seq);
    playlistIndex = indexInPlaylist;

    bpmSlider.setValue (engine.getBpm(), juce::dontSendNotification);
    timelineDirty = true;
    preloadNeighbours();
    refreshPlayerBar();
    library.saveSession (name, playlist.name);
}

void MainComponent::stepSong (int delta)
{
    const int n = playlist.items.size();
    if (n == 0)
        return;

    const int target = playlistIndex < 0 ? 0 : playlistIndex + delta;
    if (target < 0 || target >= n)
        return;

    switchToSequence (playlist.items[target], target);
}

void MainComponent::savePlaylistNow()
{
    if (playlist.name.isNotEmpty())
        library.savePlaylist (playlist);
}

void MainComponent::saveEverything (bool announce)
{
    engine.saveActiveSequence();
    savePlaylistNow();
    if (announce)
        say ("Сохранено"_ru);
}




void MainComponent::addToPlaylist (const juce::String& sequenceName)
{
    if (playlist.name.isEmpty())
        playlist.name = Library::uniqueName (library.playlistNames(), "Плейлист 1"_ru);

    playlist.items.add (sequenceName);
    savePlaylistNow();

    auto seq = engine.getActiveSequence();
    playlistIndex = seq != nullptr ? playlist.items.indexOf (seq->name) : -1;
    preloadNeighbours();
    refreshPlayerBar();
    library.saveSession (seq != nullptr ? seq->name : juce::String(), playlist.name);
}

void MainComponent::showAddTrackMenu()
{
    const auto names = library.sequenceNames();

    juce::PopupMenu m;
    m.addSectionHeader ("Добавить трек в плейлист"_ru);
    for (int i = 0; i < names.size(); ++i)
        m.addItem (100 + i, names[i]);
    if (! names.isEmpty())
        m.addSeparator();
    m.addItem (1, "Новый трек..."_ru);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (playerBar.getAddButtonScreenBounds()),
                     [safe, names] (int r)
                     {
                         if (safe == nullptr || r <= 0)
                             return;
                         if (r >= 100)
                             safe->addToPlaylist (names[r - 100]);
                         else if (r == 1)
                             safe->askText ("Новый трек"_ru, "Название:"_ru, "Трек"_ru,
                                            [safe] (juce::String n) { if (safe != nullptr) safe->createSequence (n, true); });
                     });
}

void MainComponent::createSequence (const juce::String& wantedName, bool addToList)
{
    const auto name = Library::uniqueName (library.sequenceNames(), Library::sanitize (wantedName));
    engine.saveActiveSequence();

    if (addToList)
        addToPlaylist (name);   // adds the name; the file is created below

    switchToSequence (name, playlist.items.indexOf (name));
    engine.saveActiveSequence();   // writes the (empty) file so the sequence shows up in the lists
    refreshPlayerBar();
}

void MainComponent::renameSequence()
{
    auto seq = engine.getActiveSequence();
    if (seq == nullptr)
        return;

    askText ("Переименовать трек"_ru, "Новое название:"_ru, seq->name,
             [this] (juce::String wanted)
             {
                 auto s = engine.getActiveSequence();
                 if (s == nullptr)
                     return;

                 auto names = library.sequenceNames();
                 names.removeString (s->name);
                 const auto newName = Library::uniqueName (names, Library::sanitize (wanted));
                 if (newName == s->name)
                     return;

                 engine.saveActiveSequence();
                 const auto from = s->file, to = library.sequenceFile (newName);
                 if (! from.moveFileTo (to))
                     return;
                 LyricsData::rename (from, to);

                 const auto oldName = s->name;
                 engine.renameSequenceFile (from, to);
                 library.renameInPlaylists (oldName, newName);
                 for (auto& item : playlist.items)
                     if (item == oldName)
                         item = newName;
                 refreshPlayerBar();
                 library.saveSession (newName, playlist.name);
             });
}

void MainComponent::renamePlaylist()
{
    if (playlist.name.isEmpty())
        return;

    askText ("Переименовать плейлист"_ru, "Новое название:"_ru, playlist.name,
             [this] (juce::String wanted)
             {
                 auto names = library.playlistNames();
                 names.removeString (playlist.name);
                 const auto newName = Library::uniqueName (names, Library::sanitize (wanted));
                 if (newName == playlist.name)
                     return;

                 library.playlistFile (playlist.name).deleteFile();
                 library.renamePlaylistInOrder (playlist.name, newName);
                 playlist.name = newName;
                 savePlaylistNow();
                 refreshPlayerBar();
                 if (auto s = engine.getActiveSequence())
                     library.saveSession (s->name, newName);
             });
}

void MainComponent::showDeleteMenu()
{
    auto seq = engine.getActiveSequence();
    const juce::String cur = seq != nullptr ? seq->name : juce::String();

    juce::PopupMenu m;
    m.addItem (1, "Убрать «"_ru + cur + "» из плейлиста"_ru, playlistIndex >= 0);
    m.addItem (2, "Удалить плейлист «"_ru + playlist.name + "»"_ru, playlist.name.isNotEmpty());
    m.addSeparator();
    m.addItem (3, "Удалить трек «"_ru + cur + "» с диска..."_ru, seq != nullptr);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (playerBar.getDeleteButtonScreenBounds()),
                     [safe, cur] (int r)
                     {
                         if (safe == nullptr || r <= 0)
                             return;

                         if (r == 1)
                         {
                             safe->playlist.items.remove (safe->playlistIndex);
                             safe->playlistIndex = -1;
                             safe->savePlaylistNow();
                             safe->preloadNeighbours();
                             safe->refreshPlayerBar();
                         }
                         else if (r == 2)
                         {
                             safe->confirmDelete ("Удалить плейлист «"_ru + safe->playlist.name + "»? Сами треки останутся."_ru,
                                                  [safe]
                                                  {
                                                      if (safe == nullptr)
                                                          return;
                                                      safe->library.deletePlaylist (safe->playlist.name);
                                                      safe->playlist = Playlist();
                                                      safe->playlistIndex = -1;
                                                      safe->refreshPlayerBar();
                                                      if (auto s = safe->engine.getActiveSequence())
                                                          safe->library.saveSession (s->name, {});
                                                  });
                         }
                         else if (r == 3)
                         {
                             safe->confirmDelete ("Удалить трек «"_ru + cur + "» с диска? Это нельзя отменить."_ru,
                                                  [safe, cur]
                                                  {
                                                      if (safe == nullptr)
                                                          return;

                                                      const auto file = safe->library.sequenceFile (cur);
                                                      safe->library.removeFromPlaylists (cur);
                                                      safe->playlist.items.removeString (cur);

                                                      auto others = safe->library.sequenceNames();
                                                      others.removeString (cur);
                                                      const auto next = others.isEmpty() ? juce::String ("Трек 1"_ru) : others[0];

                                                      safe->switchToSequence (next, safe->playlist.items.indexOf (next));
                                                      safe->engine.forgetSequence (file);
                                                      LyricsData::remove (file);
                                                      file.deleteFile();
                                                      safe->engine.pruneAnalysisCacheSoon();
                                                      if (others.isEmpty())
                                                          safe->engine.saveActiveSequence();
                                                      safe->refreshPlayerBar();
                                                  });
                         }
                     });
}


// ---------------------------------------------------------------- clip selection

std::vector<std::shared_ptr<Clip>> MainComponent::selectedClips() const
{
    std::vector<std::shared_ptr<Clip>> result;
    for (auto& t : engine.getTracks())
        for (auto& c : t->getClips())
            if (timelineView.selected.count (c->id) > 0)
                result.push_back (c);
    return result;
}

void MainComponent::clipPressed (const std::shared_ptr<Track>& track, const std::shared_ptr<Clip>& clip, const juce::ModifierKeys& mods)
{
    auto tracks = engine.getTracks();
    int index = 0;
    for (int i = 0; i < (int) tracks.size(); ++i)
        if (tracks[(size_t) i] == track)
            index = i;

    if (mods.isCtrlDown() || mods.isCommandDown())
    {
        if (timelineView.selected.erase (clip->id) == 0)
            timelineView.selected.insert (clip->id);
        anchorRow = index;
    }
    else if (mods.isShiftDown() && anchorRow >= 0)
    {
        // everything on the rows between the anchor and this one
        const int a = juce::jmin (anchorRow, index), b = juce::jmax (anchorRow, index);
        for (int i = a; i <= b && i < (int) tracks.size(); ++i)
            for (auto& c : tracks[(size_t) i]->getClips())
                timelineView.selected.insert (c->id);
    }
    else
    {
        if (timelineView.selected.count (clip->id) == 0)
            timelineView.selected = { clip->id };   // clicking a selected clip keeps the group, so it can be moved together
        anchorRow = index;
    }

    // remember where everything started for the drag
    dragOrigins.clear();
    for (auto& c : selectedClips())
        dragOrigins.push_back ({ c, c->start.load() });

    repaintTimeline();
}

void MainComponent::dragSelectedClips (const std::shared_ptr<Clip>& primary, double delta)
{
    if (timelineView.selected.count (primary->id) == 0 || dragOrigins.empty())
        return;

    // the group may not go earlier than the count-in bar
    double minStart = 1.0e18;
    for (auto& o : dragOrigins)
        minStart = juce::jmin (minStart, o.second);
    delta = juce::jmax (delta, -4.0 * engine.getBeatSeconds() - minStart);

    for (auto& o : dragOrigins)
        o.first->start.store (o.second + delta);

    engine.clipsChanged();
    repaintTimeline();
}

void MainComponent::emptyPressed (const juce::ModifierKeys& mods)
{
    marqueeBase.clear();
    if (mods.isCtrlDown() || mods.isCommandDown() || mods.isShiftDown())
        marqueeBase = timelineView.selected;   // add to the current selection
    else
        timelineView.selected.clear();
    repaintTimeline();
}

void MainComponent::marqueeSelect (juce::Rectangle<int> screenRect, bool finished)
{
    if (finished)
    {
        playhead.setMarquee ({});
        return;
    }

    std::set<int> ids = marqueeBase;
    for (auto* t : trackComponents)
        t->getLane().collectClipsInScreenRect (screenRect, ids);

    timelineView.selected = ids;
    playhead.setMarquee (screenRect.withPosition (playhead.getLocalPoint (nullptr, screenRect.getPosition())));
    repaintTimeline();
}

void MainComponent::selectAllClips()
{
    timelineView.selected.clear();
    for (auto& t : engine.getTracks())
        for (auto& c : t->getClips())
            timelineView.selected.insert (c->id);
    repaintTimeline();
}

void MainComponent::deleteSelectedClips()
{
    if (timelineView.selected.empty())
        return;

    for (auto& t : engine.getTracks())
    {
        std::vector<int> ids;
        for (auto& c : t->getClips())
            if (timelineView.selected.count (c->id) > 0)
                ids.push_back (c->id);
        for (int id : ids)
            engine.removeClip (t, id);
    }
    timelineView.selected.clear();
    refreshStrips();
    repaintTimeline();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress ('a', juce::ModifierKeys::ctrlModifier, 0)
        || key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0))
    {
        selectAllClips();
        return true;
    }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        deleteSelectedClips();
        return true;
    }
    if (key == juce::KeyPress::escapeKey)
    {
        timelineView.selected.clear();
        repaintTimeline();
        return true;
    }
    if (key == juce::KeyPress::spaceKey)
    {
        if (engine.isPlaying()) engine.stopAll(); else engine.playAll();   // play / stop (back to the start)
        return true;
    }
    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
    {
        stepSong (key == juce::KeyPress::leftKey ? -1 : 1);   // previous / next song
        return true;
    }
    return false;
}


// ---------------------------------------------------------------- scene / sequence files
// A scene file (.tcscene) = one composition, a sequence file (.tcseq) = a whole sequence (all compositions and
// playlists). Both are zips with relative audio paths, so they open on any PC. Loading REPLACES what has the same
// name. Plugins are stored by name and state: the other PC needs the same VST3s.

namespace
{
    // audio paths of every clip -> "audio/<unique name>", the files are added to the zip
    void packClips (juce::XmlElement& xml, juce::ZipFile::Builder& builder, std::map<juce::String, juce::String>& packed,
                    juce::StringArray& usedNames, int& missing)
    {
        for (auto* track : xml.getChildWithTagNameIterator ("TRACK"))
            for (auto* ce : track->getChildWithTagNameIterator ("CLIP"))
            {
                const auto path = ce->getStringAttribute ("file");
                juce::File f (path);
                if (! f.existsAsFile())
                {
                    ++missing;
                    continue;
                }
                auto it = packed.find (path);
                if (it == packed.end())
                {
                    auto name = Library::uniqueName (usedNames, f.getFileName());
                    usedNames.add (name);
                    builder.addFile (f, 0, "audio/" + name);
                    it = packed.emplace (path, "audio/" + name).first;
                }
                ce->setAttribute ("file", it->second);
            }
    }

    void unpackClips (juce::XmlElement& xml, const juce::File& mediaDir)
    {
        for (auto* track : xml.getChildWithTagNameIterator ("TRACK"))
            for (auto* ce : track->getChildWithTagNameIterator ("CLIP"))
            {
                const auto rel = ce->getStringAttribute ("file");
                if (rel.startsWith ("audio/"))
                    ce->setAttribute ("file", mediaDir.getChildFile (rel).getFullPathName());
            }
    }

    void addText (juce::ZipFile::Builder& builder, const juce::String& text, const juce::String& entryName)
    {
        builder.addEntry (new juce::MemoryInputStream (text.toRawUTF8(), text.getNumBytesAsUTF8(), true), 0, entryName,
                          juce::Time::getCurrentTime());
    }

    juce::String readEntry (juce::ZipFile& zip, const juce::String& entryName)
    {
        const int i = zip.getIndexOfFileName (entryName);
        if (i < 0)
            return {};
        std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
        return in != nullptr ? in->readEntireStreamAsString() : juce::String();
    }

    bool writeZip (juce::ZipFile::Builder& builder, const juce::File& target)
    {
        target.deleteFile();
        juce::FileOutputStream out (target);
        if (out.failedToOpen())
            return false;
        builder.writeToStream (out, nullptr);
        out.flush();
        return true;
    }
}

void MainComponent::exportScene (juce::String songName)
{
    if (songName.isEmpty())
        if (auto seq = engine.getActiveSequence())
            songName = seq->name;
    if (songName.isEmpty())
    {
        say ("Нет открытой композиции"_ru);
        return;
    }
    engine.saveActiveSequence();

    chooser = std::make_unique<juce::FileChooser> ("Сохранить композицию в файл"_ru,
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile (Library::sanitize (songName) + ".tcscene"),
                                                   "*.tcscene");
    juce::Component::SafePointer<MainComponent> safe (this);
    const auto songFile = library.sequenceFile (songName);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, songFile, songName] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File())
                                  return;
                              auto target = fc.getResult().withFileExtension ("tcscene");
                              auto xml = juce::XmlDocument::parse (songFile);
                              if (xml == nullptr)
                              {
                                  safe->say ("Не удалось прочитать композицию"_ru);
                                  return;
                              }

                              juce::ZipFile::Builder builder;
                              std::map<juce::String, juce::String> packed;
                              juce::StringArray used;
                              int missing = 0;
                              packClips (*xml, builder, packed, used, missing);
                              addText (builder, xml->toString(), "song.xml");
                              addText (builder, songName, "name.txt");

                              if (! writeZip (builder, target))
                              {
                                  safe->say ("Не удалось записать файл"_ru);
                                  return;
                              }
                              safe->say ("Композиция сохранена: "_ru + target.getFileName()
                                         + (missing > 0 ? " (нет файлов: "_ru + juce::String (missing) + ")" : juce::String()));
                          });
}

void MainComponent::importSceneFile (const juce::File& file)
{
    auto* safe = this;

                              juce::ZipFile zip (file);
                              if (zip.getIndexOfFileName ("song.xml") < 0)
                              {
                                  safe->say ("Это не файл композиции TC"_ru);
                                  return;
                              }

                              auto wanted = readEntry (zip, "name.txt").trim();
                              const auto name = Library::sanitize (wanted.isEmpty() ? file.getFileNameWithoutExtension() : wanted);
                              auto mediaDir = safe->engine.getDataDirectory().getChildFile ("Media").getChildFile ("scene_" + name);

                              safe->engine.saveActiveSequence();
                              const auto target = safe->library.sequenceFile (name);
                              const bool existed = target.existsAsFile();
                              safe->engine.discardSequences (target);   // the old one (even if it is open) is replaced

                              mediaDir.deleteRecursively();
                              mediaDir.createDirectory();
                              if (! zip.uncompressTo (mediaDir, true).wasOk())
                              {
                                  safe->say ("Не удалось распаковать композицию"_ru);
                                  return;
                              }
                              auto xml = juce::XmlDocument::parse (mediaDir.getChildFile ("song.xml"));
                              mediaDir.getChildFile ("song.xml").deleteFile();
                              mediaDir.getChildFile ("name.txt").deleteFile();
                              if (xml == nullptr)
                              {
                                  safe->say ("Файл повреждён"_ru);
                                  return;
                              }
                              unpackClips (*xml, mediaDir);
                              target.replaceWithText (xml->toString());

                              if (! safe->playlist.items.contains (name))
                                  safe->addToPlaylist (name);
                              safe->switchToSequence (name, safe->playlist.items.indexOf (name));
                              safe->say ((existed ? "Композиция заменена: "_ru : "Композиция загружена: "_ru) + name);
}


void MainComponent::exportSequence (juce::String project)
{
    if (project.isEmpty())
        project = library.getProject();
    if (project.isEmpty())
    {
        say ("Нет открытой секвенции"_ru);
        return;
    }
    saveEverything (false);

    chooser = std::make_unique<juce::FileChooser> ("Сохранить секвенцию в файл"_ru,
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile (Library::sanitize (project) + ".tcseq"),
                                                   "*.tcseq");
    juce::Component::SafePointer<MainComponent> safe (this);
    const auto dir = library.projectDir (project);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, dir, project] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File())
                                  return;
                              auto target = fc.getResult().withFileExtension ("tcseq");

                              juce::ZipFile::Builder builder;
                              std::map<juce::String, juce::String> packed;
                              juce::StringArray used;
                              int missing = 0, songs = 0;
                              std::vector<std::unique_ptr<juce::XmlElement>> keep;

                              for (auto& f : dir.getChildFile ("Compositions").findChildFiles (juce::File::findFiles, false, "*.xml"))
                                  if (auto xml = juce::XmlDocument::parse (f))
                                  {
                                      packClips (*xml, builder, packed, used, missing);
                                      addText (builder, xml->toString(), "Compositions/" + f.getFileName());
                                      ++songs;
                                  }
                              for (auto& f : dir.getChildFile ("Playlists").findChildFiles (juce::File::findFiles, false, "*.xml;*.txt"))
                                  addText (builder, f.loadFileAsString(), "Playlists/" + f.getFileName());
                              addText (builder, project, "name.txt");

                              if (! writeZip (builder, target))
                              {
                                  safe->say ("Не удалось записать файл"_ru);
                                  return;
                              }
                              safe->say ("Секвенция сохранена: "_ru + target.getFileName() + " (композиций: "_ru + juce::String (songs) + ")"
                                         + (missing > 0 ? ", нет файлов: "_ru + juce::String (missing) : juce::String()));
                          });
}

void MainComponent::importSequenceFile (const juce::File& file)
{
    auto* safe = this;

                              juce::ZipFile zip (file);
                              const auto stored = readEntry (zip, "name.txt").trim();
                              if (stored.isEmpty())
                              {
                                  safe->say ("Это не файл секвенции TC"_ru);
                                  return;
                              }
                              const auto name = Library::sanitize (stored);
                              auto& lib = safe->library;
                              const auto dir = lib.projectDir (name);
                              const bool existed = dir.isDirectory();
                              const bool wasCurrent = lib.getProject() == name;
                              auto mediaDir = safe->engine.getDataDirectory().getChildFile ("Media").getChildFile ("seq_" + name);

                              safe->saveEverything (false);
                              safe->engine.discardSequences (dir);    // nothing of the old one is saved back
                              dir.deleteRecursively();
                              mediaDir.deleteRecursively();
                              mediaDir.createDirectory();
                              if (! zip.uncompressTo (mediaDir, true).wasOk())
                              {
                                  safe->say ("Не удалось распаковать секвенцию"_ru);
                                  return;
                              }
                              mediaDir.getChildFile ("name.txt").deleteFile();

                              auto comps = dir.getChildFile ("Compositions");
                              auto lists = dir.getChildFile ("Playlists");
                              comps.createDirectory();
                              lists.createDirectory();
                              int songs = 0;
                              for (auto& f : mediaDir.getChildFile ("Compositions").findChildFiles (juce::File::findFiles, false, "*.xml"))
                                  if (auto xml = juce::XmlDocument::parse (f))
                                  {
                                      unpackClips (*xml, mediaDir);
                                      comps.getChildFile (f.getFileName()).replaceWithText (xml->toString());
                                      ++songs;
                                  }
                              for (auto& f : mediaDir.getChildFile ("Playlists").findChildFiles (juce::File::findFiles, false, "*"))
                                  f.copyFileTo (lists.getChildFile (f.getFileName()));
                              mediaDir.getChildFile ("Compositions").deleteRecursively();
                              mediaDir.getChildFile ("Playlists").deleteRecursively();

                              if (wasCurrent)
                              {
                                  // the open sequence was replaced: open it again from disk
                                  lib.setProject (name);
                                  safe->playlist = Playlist();
                                  safe->playlistIndex = -1;
                                  const auto pl = lib.playlistNames();
                                  if (! pl.isEmpty())
                                      lib.loadPlaylist (pl[0], safe->playlist);
                                  safe->enterProject ({});
                              }
                              else
                                  safe->switchProject (name);
                              safe->say ((existed ? "Секвенция заменена: "_ru : "Секвенция загружена: "_ru) + name
                                         + " (композиций: "_ru + juce::String (songs) + ")");
}



// ---------------------------------------------------------------- recording

void MainComponent::toggleRecording()
{
    if (engine.isRecording())
    {
        engine.stopRecording();
        recordButton.setActive (false);
        say ("Запись остановлена. Файлы: "_ru + engine.getRecordFolder().getFullPathName());
        return;
    }

    juce::String message;
    const bool ok = engine.startRecording (message);
    recordButton.setActive (ok);
    if (ok)
        say (message);
    else
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Запись"_ru, message);
}

void MainComponent::chooseRecordFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Папка для записи"_ru, engine.getRecordFolder());
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File())
                                  return;
                              safe->engine.setRecordFolder (fc.getResult());
                              safe->say ("Папка записи: "_ru + fc.getResult().getFullPathName());
                          });
}


// playlist file (.tcpl): the playlist and every composition in it (with audio)
void MainComponent::exportPlaylist (juce::String name)
{
    if (name.isEmpty())
        name = playlist.name;
    if (name.isEmpty())
    {
        say ("Нет открытого плейлиста"_ru);
        return;
    }
    saveEverything (false);

    chooser = std::make_unique<juce::FileChooser> ("Сохранить плейлист в файл"_ru,
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile (Library::sanitize (name) + ".tcpl"),
                                                   "*.tcpl");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, name] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File())
                                  return;
                              auto target = fc.getResult().withFileExtension ("tcpl");
                              Playlist p;
                              if (! safe->library.loadPlaylist (name, p))
                              {
                                  safe->say ("Не удалось прочитать плейлист"_ru);
                                  return;
                              }

                              juce::ZipFile::Builder builder;
                              std::map<juce::String, juce::String> packed;
                              juce::StringArray used;
                              int missing = 0, songs = 0;
                              for (auto& item : p.items)
                                  if (auto xml = juce::XmlDocument::parse (safe->library.sequenceFile (item)))
                                  {
                                      packClips (*xml, builder, packed, used, missing);
                                      addText (builder, xml->toString(), "Compositions/" + Library::sanitize (item) + ".xml");
                                      ++songs;
                                  }
                              addText (builder, safe->library.playlistFile (name).loadFileAsString(), "playlist.xml");
                              addText (builder, name, "name.txt");

                              if (! writeZip (builder, target))
                              {
                                  safe->say ("Не удалось записать файл"_ru);
                                  return;
                              }
                              safe->say ("Плейлист сохранён: "_ru + target.getFileName() + " (композиций: "_ru + juce::String (songs) + ")"
                                         + (missing > 0 ? ", нет файлов: "_ru + juce::String (missing) : juce::String()));
                          });
}

void MainComponent::importPlaylistFile (const juce::File& file)
{
    juce::ZipFile zip (file);
    const auto stored = readEntry (zip, "name.txt").trim();
    const auto listXml = readEntry (zip, "playlist.xml");
    if (stored.isEmpty() || listXml.isEmpty())
    {
        say ("Это не файл плейлиста TC"_ru);
        return;
    }
    const auto name = Library::sanitize (stored);
    const bool existed = library.playlistFile (name).existsAsFile();
    auto mediaDir = engine.getDataDirectory().getChildFile ("Media").getChildFile ("pl_" + name);

    saveEverything (false);
    mediaDir.deleteRecursively();
    mediaDir.createDirectory();
    if (! zip.uncompressTo (mediaDir, true).wasOk())
    {
        say ("Не удалось распаковать плейлист"_ru);
        return;
    }

    const auto active = engine.getActiveSequence();
    const juce::String activeName = active != nullptr ? active->name : juce::String();
    bool activeReplaced = false;
    int songs = 0;
    for (auto& f : mediaDir.getChildFile ("Compositions").findChildFiles (juce::File::findFiles, false, "*.xml"))
        if (auto xml = juce::XmlDocument::parse (f))
        {
            const auto song = f.getFileNameWithoutExtension();
            const auto target = library.sequenceFile (song);
            engine.discardSequences (target);     // the same-name composition is replaced
            activeReplaced = activeReplaced || song == activeName;
            unpackClips (*xml, mediaDir);
            target.replaceWithText (xml->toString());
            ++songs;
        }
    mediaDir.getChildFile ("Compositions").deleteRecursively();
    mediaDir.getChildFile ("playlist.xml").deleteFile();
    mediaDir.getChildFile ("name.txt").deleteFile();

    library.playlistFile (name).replaceWithText (listXml);
    Playlist p;
    if (library.loadPlaylist (name, p))
        playlist = p;
    if (activeReplaced || engine.getActiveSequence() == nullptr)
        switchToSequence (playlist.items.isEmpty() ? activeName : playlist.items[0], 0);
    else
    {
        playlistIndex = playlist.items.indexOf (activeName);
        preloadNeighbours();
        refreshPlayerBar();
    }
    say ((existed ? "Плейлист заменён: "_ru : "Плейлист загружен: "_ru) + name + " (композиций: "_ru + juce::String (songs) + ")");
}

// one "import" for everything: the kind is taken from the file
void MainComponent::importFromFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Импорт"_ru, juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   "*.tcseq;*.tcpl;*.tcscene");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || ! fc.getResult().existsAsFile())
                                  return;
                              const auto file = fc.getResult();
                              const auto ext = file.getFileExtension().toLowerCase();
                              if (ext == ".tcseq")
                                  safe->importSequenceFile (file);
                              else if (ext == ".tcpl")
                                  safe->importPlaylistFile (file);
                              else
                                  safe->importSceneFile (file);
                          });
}

// the save button of the player bar: saves to a file what the current tab is about (selected row, else the open one)
void MainComponent::saveSelectionToFile (PlayerBar::Tab tab)
{
    const auto ids = playerBar.getSelectedIds();
    const int id = ids.empty() ? -1 : ids.front();
    if (tab == PlayerBar::Tab::Seq)
        exportSequence (id >= 0 && id < projectRows.size() ? projectRows[id] : juce::String());
    else if (tab == PlayerBar::Tab::Playlist)
        exportPlaylist (id >= 0 && id < playlistRows.size() ? playlistRows[id] : juce::String());
    else
        exportScene (id >= 0 && id < playlist.items.size() ? playlist.items[id] : juce::String());
}
