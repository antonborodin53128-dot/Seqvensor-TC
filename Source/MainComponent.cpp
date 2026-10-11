#include "MainComponent.h"
#include "TempoDetect.h"

// ---------------------------------------------------------------- plugin window

PluginWindow::PluginWindow (juce::AudioPluginInstance& pluginToShow, std::function<void (PluginWindow*)> onCloseRequested)
    : juce::DocumentWindow (pluginToShow.getName(), Pal::surface, juce::DocumentWindow::closeButton),
      plugin (&pluginToShow),
      onClose (std::move (onCloseRequested))
{
    setUsingNativeTitleBar (true);

    juce::AudioProcessorEditor* editor = plugin->hasEditor() ? plugin->createEditorIfNeeded() : nullptr;
    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor (*plugin);

    setContentOwned (editor, true);
    setResizable (editor->isResizable(), false);
    setAlwaysOnTop (true);
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

    bpmSlider.setSliderStyle (juce::Slider::IncDecButtons);
    bpmSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
    bpmSlider.setRange (20.0, 400.0, 0.1);
    bpmSlider.setValue (engine.getBpm(), juce::dontSendNotification);
    bpmSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 86, 32);
    bpmSlider.setTextValueSuffix (" BPM");
    bpmSlider.setTooltip ("Темп проекта. Его получают плагины (дилей и др.) и метроном."_ru);
    bpmSlider.onValueChange = [this] { engine.setBpm (bpmSlider.getValue()); };
    addAndMakeVisible (bpmSlider);

    tapButton.onClick = [this] { tapTempo(); };
    addAndMakeVisible (tapButton);

    keyLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    keyLabel.setJustificationType (juce::Justification::centred);
    keyLabel.setTooltip ("Автоматическое определение тональности появится в следующей версии."_ru);
    addAndMakeVisible (keyLabel);

    analysisButton.setTooltip ("Настройки определения темпа и тональности (каналы-источники)"_ru);
    analysisButton.onClick = [this] { showAnalysisSettings(); };
    addAndMakeVisible (analysisButton);

    playAllButton.setTooltip ("Играть. Если метроном включён хотя бы на одной дорожке - сначала 4 удара отсчёта (такт перед первым)."_ru);
    playAllButton.onClick = [this] { engine.playAll(); };
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

    viewport.setViewedComponent (&trackList, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    ruler.onSeek = [this] (double t) { seekTimeline (t); };
    ruler.onZoom = [this] (float d) { zoomTimeline (d); };
    addAndMakeVisible (ruler);
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
    playerBar.cb.sequenceMenu = [this] { showSequenceMenu(); };
    playerBar.cb.sequenceSave = [this] { saveEverything (true); };
    playerBar.cb.sequenceNew = [this] { askText ("Новая секвенция"_ru, "Название:"_ru, "Секвенция"_ru,
                                                  [this] (juce::String n) { createSequence (n, false); }); };
    playerBar.cb.playlistMenu = [this] { showPlaylistMenu(); };
    playerBar.cb.playlistSave = [this] { saveEverything (true); };
    playerBar.cb.playlistDelete = [this] { showDeleteMenu(); };
    playerBar.cb.playlistAdd = [this] { showAddTrackMenu(); };
    playerBar.cb.plateClick = [this] { showSongsMenu(); };
    playerBar.cb.previous = [this] { stepSong (-1); };
    playerBar.cb.next = [this] { stepSong (1); };

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
    pluginWindows.clear();
    trackComponents.clear();
    engine.shutdown();
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Pal::window);
    const int topH = 62 + PlayerBar::barHeight;
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
    const int groupW = 170 + 8 + 56 + 20 + 110 + 8 + 44 + 30 + 72 + 6 + 64 + 6 + 64;
    auto group = juce::Rectangle<int> (groupW, rowA.getHeight()).withCentre ({ getWidth() / 2, rowA.getCentreY() });
    bpmSlider.setBounds (group.removeFromLeft (170));
    group.removeFromLeft (8);
    tapButton.setBounds (group.removeFromLeft (56));
    group.removeFromLeft (20);
    keyLabel.setBounds (group.removeFromLeft (110));
    group.removeFromLeft (8);
    analysisButton.setBounds (group.removeFromLeft (44));
    group.removeFromLeft (30);
    playAllButton.setBounds (group.removeFromLeft (72));
    group.removeFromLeft (6);
    pauseAllButton.setBounds (group.removeFromLeft (64));
    group.removeFromLeft (6);
    stopAllButton.setBounds (group.removeFromLeft (64));

    playerBar.setBounds (area.removeFromTop (PlayerBar::barHeight));

    area.removeFromTop (28);   // ruler row (placed in layoutTimelineParts)
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
    playhead.setBounds (laneX, top, laneW, rulerH + viewport.getHeight());
    timelineDirty = true;
}

void MainComponent::layoutTracks()
{
    const int w = juce::jmax (TrackComponent::headerWidth + 100, viewport.getMaximumVisibleWidth());
    int y = 0;
    for (auto* t : trackComponents)
    {
        t->setBounds (0, y, w, TrackComponent::rowHeight);
        y += TrackComponent::rowHeight;
    }

    addTrackButton.setBounds (8, y + 8, TrackComponent::headerWidth - 20, 34);
    y += 50;
    trackList.setSize (w, juce::jmax (y, viewport.getHeight()));
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
    cb.slotMenu = [this] (std::shared_ptr<Track> t, int i, juce::Component* c) { showSlotMenu (t, i, c); };
    cb.outputMenu = [this] (std::shared_ptr<Track> t, juce::Component* c) { showOutputMenu (t, c); };

    LaneComponent::Callbacks lane;
    lane.seek = [this] (double t) { seekTimeline (t); };
    lane.zoom = [this] (float d) { zoomTimeline (d); };
    lane.clipMenu = [this] (std::shared_ptr<Track> t, std::shared_ptr<Clip> c, juce::Point<int> p) { showClipMenu (t, c, p); };
    lane.laneMenu = [this] (std::shared_ptr<Track> t, double time, juce::Point<int> p) { showLaneMenu (t, time, p); };
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

void MainComponent::refreshStrips()
{
    for (auto* t : trackComponents)
        t->refreshAll();
}

void MainComponent::addTrack()
{
    const int count = (int) engine.getTracks().size();
    auto track = engine.addTrack ("Дорожка "_ru + juce::String (count + 1));
    track->inputL = count;   // successive tracks default to successive inputs
    refreshStrips();
}

// ---------------------------------------------------------------- timeline

void MainComponent::repaintTimeline()
{
    ruler.repaint();
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
    for (auto& path : files)
        engine.addClipFromFile (track, juce::File (path), time,
                                [safe, path] (std::shared_ptr<Clip> clip, const juce::String& error)
                                {
                                    if (safe == nullptr)
                                        return;
                                    if (clip == nullptr)
                                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "TC",
                                                                                error + ":\n"_ru + path);
                                    safe->refreshStrips();
                                    safe->repaintTimeline();
                                });
    refreshStrips();
    repaintTimeline();
}

void MainComponent::showLaneMenu (const std::shared_ptr<Track>& track, double time, juce::Point<int> screenPos)
{
    juce::PopupMenu m;
    m.addItem (1, "Добавить аудио сюда..."_ru);
    m.addSeparator();
    m.addItem (2, "Привязка к сетке"_ru, true, timelineView.snap);

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
                     });
}

void MainComponent::showClipMenu (const std::shared_ptr<Track>& track, const std::shared_ptr<Clip>& clip, juce::Point<int> screenPos)
{
    const int total = (int) clip->data->lengthSeconds();
    juce::PopupMenu m;
    m.addItem (100, clip->data->name + "   ("_ru + juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2) + ")", false);
    m.addSeparator();
    m.addItem (1, "Определить темп (только показать)"_ru);
    m.addItem (2, "Поставить на такт 1"_ru);
    m.addItem (3, "Поставить на такт раньше (в отсчёт)"_ru);
    m.addSeparator();
    m.addItem (4, "Повтор"_ru, true, clip->loop.load());
    m.addItem (5, "Привязка к сетке"_ru, true, timelineView.snap);
    m.addItem (6, "Заменить файл..."_ru);
    m.addSeparator();
    m.addItem (7, "Удалить"_ru);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                     [safe, track, clip] (int r)
                     {
                         if (safe == nullptr || r <= 0)
                             return;

                         auto& eng = safe->engine;
                         switch (r)
                         {
                             case 1: safe->detectClipTempo (clip); break;
                             case 2: clip->start.store (0.0); eng.clipsChanged(); break;
                             case 3: clip->start.store (-4.0 * eng.getBeatSeconds()); eng.clipsChanged(); break;
                             case 4: clip->loop = ! clip->loop.load(); eng.clipsChanged(); break;
                             case 5: safe->timelineView.snap = ! safe->timelineView.snap; break;
                             case 6:
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
                                                                 safe->engine.addClipFromFile (track, fc.getResult(), start,
                                                                     [safe, track, clip, loop] (std::shared_ptr<Clip> fresh, const juce::String&)
                                                                     {
                                                                         if (safe == nullptr || fresh == nullptr)
                                                                             return;
                                                                         fresh->loop = loop;
                                                                         safe->engine.removeClip (track, clip->id);
                                                                         safe->repaintTimeline();
                                                                     });
                                                             });
                                 break;
                             }
                             case 7: eng.removeClip (track, clip->id); break;
                             default: break;
                         }
                         safe->repaintTimeline();
                     });
}

void MainComponent::detectClipTempo (const std::shared_ptr<Clip>& clip)
{
    statusLabel.setText ("Определяю темп..."_ru, juce::dontSendNotification);
    juce::Component::SafePointer<MainComponent> safe (this);
    auto data = clip->data;

    juce::Thread::launch ([safe, data]
    {
        std::vector<float> mono;
        data->getMono (mono, (int) (data->sampleRate * 150.0));
        const double bpm = detectTempo (mono, data->sampleRate);

        juce::MessageManager::callAsync ([safe, bpm]
        {
            if (safe == nullptr)
                return;
            if (bpm > 0.0)
            {
                safe->statusLabel.setText ("Темп файла: "_ru + juce::String (bpm, 2) + " (темп проекта не изменён)"_ru,
                                           juce::dontSendNotification);
            }
            else
                safe->statusLabel.setText ("Темп определить не удалось"_ru, juce::dontSendNotification);
            safe->repaintTimeline();
        });
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
        menu.addItem (3, "Обход (bypass)"_ru, true, slot->bypass.load());
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
    pluginWindows.add (new PluginWindow (*slot->plugin, [safe] (PluginWindow* w)
    {
        juce::MessageManager::callAsync ([safe, w]
        {
            if (safe != nullptr)
                safe->pluginWindows.removeObject (w, true);
        });
    }));
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
    menu.addItem (5, "Поверх всех окон"_ru, true, pinned);
    menu.addItem (7, "Привязка клипов к сетке"_ru, true, timelineView.snap);
    menu.addItem (6, "Сохранить секвенцию и плейлист"_ru);

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
                                case 5: safe->setPinned (! safe->pinned); break;
                                case 6: safe->saveEverything (true); break;
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
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Темп и тональность"_ru,
                                            "Здесь будет выбор каналов-источников для автоматического определения "_ru
                                            "темпа и тональности. Эта функция появится в следующей версии.\n\n"_ru
                                            "Пока темп задаётся вручную или кнопкой Tap; правая кнопка по клипу - "_ru
                                            "Определить темп (только показывает значение)."_ru);
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
            engine.setBpm (60000.0 / avg);
            bpmSlider.setValue (engine.getBpm(), juce::dontSendNotification);
        }
    }
}

// ---------------------------------------------------------------- timers / listeners

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &engine.knownPlugins)
        engine.savePluginList();
}

// Runs once per screen refresh. The audio clock only advances once per audio block, so the displayed position
// is extrapolated with the wall clock and gently pulled towards the real one: smooth, never jumps back.
void MainComponent::updateTimeline()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double engineClock = engine.getClockSeconds();
    if (engineClock != engineClockSeen)
    {
        engineClockSeen = engineClock;
        engineClockStamp = now;
    }

    if (engine.isPlaying() && ! engine.isPaused())
    {
        const double predicted = engineClock + (now - engineClockStamp);
        if (std::abs (predicted - displayClock) > 0.25)
            displayClock = predicted;
        else
            displayClock += (now - lastFrameStamp) + (predicted - displayClock - (now - lastFrameStamp)) * 0.1;
    }
    else
        displayClock = engineClock;

    lastFrameStamp = now;
    timelineView.clock = displayClock;

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

void MainComponent::timerCallback()
{
    for (auto* t : trackComponents)
        t->updateLive();

    playAllButton.setActive (engine.isPlaying());
    pauseAllButton.setActive (engine.isPaused());
    if (std::abs (bpmSlider.getValue() - engine.getBpm()) > 0.001 && ! bpmSlider.isMouseButtonDown())
        bpmSlider.setValue (engine.getBpm(), juce::dontSendNotification);

    if (++slowCounter >= 15)   // twice a second
    {
        slowCounter = 0;
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

juce::PopupMenu::Options MainComponent::optionsBelow (juce::Rectangle<int> r)
{
    return juce::PopupMenu::Options().withTargetScreenArea (playerBar.localAreaToGlobal (r))
                                     .withMinimumWidth (r.getWidth())
                                     .withPreferredPopupDirection (juce::PopupMenu::Options::PopupDirection::downwards);
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
    // projects of the older versions: one project.xml -> the first sequence
    if (library.sequenceNames().isEmpty() && engine.getLegacyProjectFile().existsAsFile())
        engine.getLegacyProjectFile().copyFileTo (library.sequenceFile ("Секвенция 1"_ru));

    juce::String seqName, plName;
    library.loadSession (seqName, plName);

    if (plName.isNotEmpty())
        library.loadPlaylist (plName, playlist);

    auto names = library.sequenceNames();
    if (seqName.isEmpty() || ! names.contains (seqName))
        seqName = names.isEmpty() ? juce::String ("Секвенция 1"_ru) : names[0];

    switchToSequence (seqName, playlist.items.indexOf (seqName));
    if (names.isEmpty())
        engine.saveActiveSequence();   // creates the file
}

void MainComponent::refreshPlayerBar()
{
    auto seq = engine.getActiveSequence();
    const juce::String current = seq != nullptr ? seq->name : juce::String();
    const int n = playlist.items.size();

    juce::String next = "-";
    if (playlistIndex >= 0 && playlistIndex + 1 < n)
        next = playlist.items[playlistIndex + 1];

    playerBar.setInfo (current, playlist.name.isNotEmpty() ? playlist.name : juce::String ("Нет плейлиста"_ru),
                       current, next,
                       playlistIndex >= 0 ? juce::String (playlistIndex + 1) + " / " + juce::String (n) : juce::String());
    playerBar.setCanStep (playlistIndex > 0, playlistIndex >= 0 && playlistIndex + 1 < n);
}

void MainComponent::preloadNeighbours()
{
    std::vector<juce::File> keep;
    if (auto seq = engine.getActiveSequence())
        keep.push_back (seq->file);

    if (playlistIndex >= 0)
        for (int i : { playlistIndex - 1, playlistIndex + 1 })
            if (i >= 0 && i < playlist.items.size())
            {
                auto f = library.sequenceFile (playlist.items[i]);
                if (f.existsAsFile())
                {
                    engine.obtainSequence (f);   // loads clips and plugins in the background
                    keep.push_back (f);
                }
            }

    engine.dropSequences (keep);
}

void MainComponent::switchToSequence (const juce::String& name, int indexInPlaylist)
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

void MainComponent::showSongsMenu()
{
    if (playlist.items.isEmpty())
    {
        showPlaylistMenu();
        return;
    }

    juce::PopupMenu m;
    for (int i = 0; i < playlist.items.size(); ++i)
        m.addItem (100 + i, juce::String (i + 1) + ".  " + playlist.items[i], true, i == playlistIndex);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (optionsBelow (playerBar.getPlateRect()),
                     [safe] (int r)
                     {
                         if (safe != nullptr && r >= 100)
                             safe->switchToSequence (safe->playlist.items[r - 100], r - 100);
                     });
}

void MainComponent::showSequenceMenu()
{
    const auto names = library.sequenceNames();
    auto seq = engine.getActiveSequence();

    juce::PopupMenu m;
    for (int i = 0; i < names.size(); ++i)
        m.addItem (100 + i, names[i], true, seq != nullptr && names[i] == seq->name);
    m.addSeparator();
    m.addItem (1, "Новая секвенция..."_ru);
    m.addItem (2, "Переименовать..."_ru);
    m.addItem (3, "Сохранить"_ru);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (optionsBelow (playerBar.getSequenceRect()),
                     [safe, names] (int r)
                     {
                         if (safe == nullptr || r <= 0)
                             return;
                         if (r >= 100)
                             safe->switchToSequence (names[r - 100], safe->playlist.items.indexOf (names[r - 100]));
                         else if (r == 1)
                             safe->askText ("Новая секвенция"_ru, "Название:"_ru, "Секвенция"_ru,
                                            [safe] (juce::String n) { if (safe != nullptr) safe->createSequence (n, false); });
                         else if (r == 2)
                             safe->renameSequence();
                         else if (r == 3)
                             safe->saveEverything (true);
                     });
}

void MainComponent::showPlaylistMenu()
{
    const auto names = library.playlistNames();

    juce::PopupMenu m;
    for (int i = 0; i < names.size(); ++i)
        m.addItem (100 + i, names[i], true, names[i] == playlist.name);
    if (! names.isEmpty())
        m.addSeparator();
    m.addItem (1, "Новый плейлист..."_ru);
    m.addItem (2, "Переименовать..."_ru, playlist.name.isNotEmpty());

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (optionsBelow (playerBar.getPlaylistRect()),
                     [safe, names] (int r)
                     {
                         if (safe == nullptr || r <= 0)
                             return;

                         if (r >= 100)
                         {
                             safe->savePlaylistNow();
                             Playlist p;
                             if (! safe->library.loadPlaylist (names[r - 100], p))
                                 return;
                             safe->playlist = p;

                             auto seq = safe->engine.getActiveSequence();
                             const int at = seq != nullptr ? p.items.indexOf (seq->name) : -1;
                             if (at < 0 && ! p.items.isEmpty())
                                 safe->switchToSequence (p.items[0], 0);
                             else
                             {
                                 safe->playlistIndex = at;
                                 safe->preloadNeighbours();
                                 safe->refreshPlayerBar();
                                 safe->library.saveSession (seq != nullptr ? seq->name : juce::String(), p.name);
                             }
                         }
                         else if (r == 1)
                             safe->askText ("Новый плейлист"_ru, "Название:"_ru, "Плейлист"_ru,
                                            [safe] (juce::String n)
                                            {
                                                if (safe == nullptr)
                                                    return;
                                                safe->savePlaylistNow();
                                                safe->playlist = Playlist();
                                                safe->playlist.name = Library::uniqueName (safe->library.playlistNames(), Library::sanitize (n));
                                                safe->playlistIndex = -1;
                                                safe->savePlaylistNow();
                                                safe->refreshPlayerBar();
                                                if (auto s = safe->engine.getActiveSequence())
                                                    safe->library.saveSession (s->name, safe->playlist.name);
                                            });
                         else if (r == 2)
                             safe->renamePlaylist();
                     });
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
    m.addSectionHeader ("Добавить в плейлист"_ru);
    for (int i = 0; i < names.size(); ++i)
        m.addItem (100 + i, names[i]);
    if (! names.isEmpty())
        m.addSeparator();
    m.addItem (1, "Новая пустая секвенция..."_ru);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (playerBar.localAreaToGlobal (playerBar.getAddButtonRect())),
                     [safe, names] (int r)
                     {
                         if (safe == nullptr || r <= 0)
                             return;
                         if (r >= 100)
                             safe->addToPlaylist (names[r - 100]);
                         else if (r == 1)
                             safe->askText ("Новая секвенция"_ru, "Название:"_ru, "Секвенция"_ru,
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

    askText ("Переименовать секвенцию"_ru, "Новое название:"_ru, seq->name,
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
    m.addItem (3, "Удалить секвенцию «"_ru + cur + "» с диска..."_ru, seq != nullptr);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (playerBar.localAreaToGlobal (playerBar.getDeleteButtonRect())),
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
                             safe->confirmDelete ("Удалить плейлист «"_ru + safe->playlist.name + "»? Сами секвенции останутся."_ru,
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
                             safe->confirmDelete ("Удалить секвенцию «"_ru + cur + "» с диска? Это нельзя отменить."_ru,
                                                  [safe, cur]
                                                  {
                                                      if (safe == nullptr)
                                                          return;

                                                      const auto file = safe->library.sequenceFile (cur);
                                                      safe->library.removeFromPlaylists (cur);
                                                      safe->playlist.items.removeString (cur);

                                                      auto others = safe->library.sequenceNames();
                                                      others.removeString (cur);
                                                      const auto next = others.isEmpty() ? juce::String ("Секвенция 1"_ru) : others[0];

                                                      safe->switchToSequence (next, safe->playlist.items.indexOf (next));
                                                      safe->engine.forgetSequence (file);
                                                      file.deleteFile();
                                                      if (others.isEmpty())
                                                          safe->engine.saveActiveSequence();
                                                      safe->refreshPlayerBar();
                                                  });
                         }
                     });
}
