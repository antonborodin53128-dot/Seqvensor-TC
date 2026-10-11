#include "MainComponent.h"
#include "TempoDetect.h"

// ---------------------------------------------------------------- plugin window

PluginWindow::PluginWindow (juce::AudioPluginInstance& pluginToShow, std::function<void (PluginWindow*)> onCloseRequested)
    : juce::DocumentWindow (pluginToShow.getName(), juce::Colour (0xff20242a), juce::DocumentWindow::closeButton),
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
        o.dialogBackgroundColour = juce::Colour (0xff20242a);
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
{
    lookAndFeel.setColourScheme (juce::LookAndFeel_V4::getMidnightColourScheme());
    lookAndFeel.setColour (juce::ResizableWindow::backgroundColourId, juce::Colour (0xff181b1f));
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
    addTrackButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff23272c));
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

    engine.loadProject();
    bpmSlider.setValue (engine.getBpm(), juce::dontSendNotification);
    rebuildTracks();
    startTimerHz (60);
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
    g.fillAll (juce::Colour (0xff181b1f));
    g.setColour (juce::Colour (0xff20242a));
    g.fillRect (getLocalBounds().removeFromTop (96));
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    auto fullBar = area.removeFromTop (96);
    auto bar = fullBar.removeFromTop (52).reduced (12, 8);

    menuButton.setBounds (bar.removeFromLeft (40));
    cpuLabel.setBounds (bar.removeFromRight (110));

    // tempo + key + gear, centred in the whole window
    const int groupW = 170 + 8 + 56 + 30 + 130 + 8 + 38;
    auto group = juce::Rectangle<int> (groupW, bar.getHeight()).withCentre ({ getWidth() / 2, bar.getCentreY() });
    bpmSlider.setBounds (group.removeFromLeft (170));
    group.removeFromLeft (8);
    tapButton.setBounds (group.removeFromLeft (56));
    group.removeFromLeft (30);
    keyLabel.setBounds (group.removeFromLeft (130));
    group.removeFromLeft (8);
    analysisButton.setBounds (group.removeFromLeft (38));

    // global transport under the tempo
    auto transportRow = juce::Rectangle<int> (46 * 3 + 8, 34).withCentre ({ getWidth() / 2, fullBar.getY() + 20 });
    playAllButton.setBounds (transportRow.removeFromLeft (46));
    transportRow.removeFromLeft (4);
    pauseAllButton.setBounds (transportRow.removeFromLeft (46));
    transportRow.removeFromLeft (4);
    stopAllButton.setBounds (transportRow.removeFromLeft (46));

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
    menu.addItem (6, "Сохранить проект"_ru);

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
                                case 6: safe->engine.saveProject(); break;
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

void MainComponent::timerCallback()
{
    for (auto* t : trackComponents)
        t->updateLive();

    timelineView.clock = engine.getClockSeconds();
    if (timelineDirty || timelineView.clock != lastClock || timelineView.span != lastSpan || engine.getBpm() != lastBpm)
    {
        lastClock = timelineView.clock;
        lastSpan = timelineView.span;
        lastBpm = engine.getBpm();
        timelineDirty = false;
        repaintTimeline();
    }
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
        engine.saveProject();
    }
}
