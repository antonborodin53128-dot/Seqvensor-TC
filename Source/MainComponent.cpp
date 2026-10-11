#include "MainComponent.h"

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

    titleLabel.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xff2fb5a2));
    addAndMakeVisible (titleLabel);

    audioButton.onClick = [this] { showAudioSettings(); };
    pluginsButton.onClick = [this] { showPluginManager(); };
    addAndMakeVisible (audioButton);
    addAndMakeVisible (pluginsButton);

    pinButton.setToggleState (true, juce::dontSendNotification);
    pinButton.onClick = [this] { setPinned (pinButton.getToggleState()); };
    addAndMakeVisible (pinButton);

    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (statusLabel);

    for (auto* l : { &bpmCaption, &keyCaption })
    {
        l->setFont (juce::FontOptions (11.0f, juce::Font::bold));
        l->setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (l);
    }

    bpmSlider.setSliderStyle (juce::Slider::IncDecButtons);
    bpmSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
    bpmSlider.setRange (20.0, 400.0, 0.1);
    bpmSlider.setValue (engine.getBpm(), juce::dontSendNotification);
    bpmSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 32);
    bpmSlider.onValueChange = [this] { engine.setBpm (bpmSlider.getValue()); };
    addAndMakeVisible (bpmSlider);

    tapButton.onClick = [this] { tapTempo(); };
    addAndMakeVisible (tapButton);

    keyValue.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    keyValue.setTooltip ("Автоматическое определение тональности появится в следующей версии."_ru);
    addAndMakeVisible (keyValue);

    analysisButton.setTooltip ("Настройки определения темпа и тональности (каналы-источники)"_ru);
    analysisButton.onClick = [this] { showAnalysisSettings(); };
    addAndMakeVisible (analysisButton);

    addLiveButton.onClick = [this] { addLiveTrack(); };
    addPlayerButton.onClick = [this] { addPlayerTrack(); };
    addAndMakeVisible (addLiveButton);
    addAndMakeVisible (addPlayerButton);

    viewport.setViewedComponent (&trackList, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    emptyHint.setText ("Добавьте дорожку: живой вход для голоса или плейбэк из файла. Затем нажмите «+ эффект» и выберите VST3-плагин "_ru
                       "(если список пуст, сначала откройте «Плагины» и просканируйте)."_ru,
                       juce::dontSendNotification);
    emptyHint.setJustificationType (juce::Justification::centred);
    emptyHint.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (emptyHint);

    setSize (1000, 660);

    engine.loadProject();
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
    g.fillAll (juce::Colour (0xff181b1f));

    // project panel background
    auto panel = getLocalBounds().reduced (12, 0).withTrimmedTop (56).removeFromTop (64).toFloat();
    g.setColour (juce::Colour (0xff20242a));
    g.fillRoundedRectangle (panel, 8.0f);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (12, 8);

    auto header = area.removeFromTop (40);
    titleLabel.setBounds (header.removeFromLeft (60));
    audioButton.setBounds (header.removeFromLeft (80).reduced (0, 4));
    header.removeFromLeft (6);
    pluginsButton.setBounds (header.removeFromLeft (90).reduced (0, 4));
    header.removeFromLeft (12);
    pinButton.setBounds (header.removeFromLeft (170));
    statusLabel.setBounds (header);

    area.removeFromTop (8);

    auto panel = area.removeFromTop (64).reduced (14, 8);
    auto tempoArea = panel.removeFromLeft (360);
    bpmCaption.setBounds (tempoArea.removeFromTop (14));
    bpmSlider.setBounds (tempoArea.removeFromLeft (190));
    tempoArea.removeFromLeft (8);
    tapButton.setBounds (tempoArea.removeFromLeft (70).reduced (0, 2));

    panel.removeFromLeft (30);
    auto keyArea = panel.removeFromLeft (300);
    keyCaption.setBounds (keyArea.removeFromTop (14));
    analysisButton.setBounds (keyArea.removeFromRight (36).reduced (0, 2));
    keyValue.setBounds (keyArea);

    area.removeFromTop (8);

    auto bottom = area.removeFromBottom (36);
    addLiveButton.setBounds (bottom.removeFromLeft (300));
    bottom.removeFromLeft (8);
    addPlayerButton.setBounds (bottom.removeFromLeft (200));
    area.removeFromBottom (6);

    viewport.setBounds (area);
    emptyHint.setBounds (area.reduced (40, 20));
    layoutTracks();
}

void MainComponent::layoutTracks()
{
    const int w = juce::jmax (200, viewport.getMaximumVisibleWidth());
    int y = 0;
    for (auto* t : trackComponents)
    {
        t->setBounds (0, y, w, TrackComponent::preferredHeight);
        y += TrackComponent::preferredHeight + 4;
    }
    trackList.setSize (w, juce::jmax (y, viewport.getHeight()));
    emptyHint.setVisible (trackComponents.isEmpty());
}

// ---------------------------------------------------------------- tracks

void MainComponent::rebuildTracks()
{
    trackComponents.clear();

    TrackComponent::Callbacks cb;
    cb.remove = [this] (std::shared_ptr<Track> t)
    {
        for (int i = 0; i < t->getNumPlugins(); ++i)
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
    cb.openEditor = [this] (std::shared_ptr<Track> t, int i) { openPluginEditor (t, i); };
    cb.addPlugin = [this] (std::shared_ptr<Track> t, juce::Component* c) { showPluginMenu (t, c); };
    cb.pickFile = [this] (std::shared_ptr<Track> t) { pickPlayerFile (t); };
    cb.pluginMenu = [this] (std::shared_ptr<Track> t, int i, juce::Component* c) { showSlotMenu (t, i, c); };

    for (auto& t : engine.getTracks())
    {
        auto* tc = new TrackComponent (engine, t, cb);
        trackComponents.add (tc);
        trackList.addAndMakeVisible (tc);
    }
    layoutTracks();
}

void MainComponent::addLiveTrack()
{
    int liveCount = 0;
    for (auto& t : engine.getTracks())
        if (t->kind == Track::Kind::Live)
            ++liveCount;

    auto track = engine.addTrack (Track::Kind::Live, "Вокал "_ru + juce::String (liveCount + 1));
    track->inputL = liveCount;   // successive tracks default to successive inputs
    rebuildTracks();
}

void MainComponent::addPlayerTrack()
{
    int count = 0;
    for (auto& t : engine.getTracks())
        if (t->kind == Track::Kind::Player)
            ++count;

    auto track = engine.addTrack (Track::Kind::Player, "Плейбэк "_ru + juce::String (count + 1));
    rebuildTracks();
    pickPlayerFile (track);
}

void MainComponent::pickPlayerFile (const std::shared_ptr<Track>& track)
{
    chooser = std::make_unique<juce::FileChooser> ("Выберите аудиофайл"_ru, juce::File(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    juce::Component::SafePointer<MainComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe, track] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr)
                                  return;
                              const auto file = fc.getResult();
                              if (file == juce::File())
                                  return;
                              if (! track->loadFile (file))
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "TC",
                                                                          "Не удалось открыть файл:\n"_ru + file.getFullPathName());
                              else
                                  track->name = file.getFileNameWithoutExtension();
                              safe->rebuildTracks();
                          });
}

// ---------------------------------------------------------------- plugins

void MainComponent::showPluginMenu (const std::shared_ptr<Track>& track, juce::Component* anchor)
{
    std::vector<juce::PluginDescription> effects;
    for (auto& d : engine.knownPlugins.getTypes())
        if (! d.isInstrument)
            effects.push_back (d);

    std::sort (effects.begin(), effects.end(), [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
    {
        return a.name.compareIgnoreCase (b.name) < 0;
    });

    juce::PopupMenu menu;
    if (effects.empty())
    {
        menu.addItem (1000000, "Плагинов нет — откройте «Плагины» и просканируйте"_ru, false);
    }
    else
    {
        for (size_t i = 0; i < effects.size(); ++i)
            menu.addItem ((int) i + 1, effects[i].name + "   (" + effects[i].manufacturerName + ")");
    }

    juce::Component::SafePointer<MainComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor),
                        [safe, track, effects] (int result)
                        {
                            if (safe == nullptr || result <= 0 || result > (int) effects.size())
                                return;

                            const auto desc = effects[(size_t) result - 1];
                            safe->engine.addPluginToTrack (track, desc, [safe, desc] (bool ok, const juce::String& error)
                            {
                                if (safe == nullptr)
                                    return;
                                if (! ok)
                                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "TC",
                                                                            "Не удалось загрузить плагин «"_ru + desc.name + "»:\n"_ru + error);
                                safe->rebuildTracks();
                            });
                        });
}

void MainComponent::showSlotMenu (const std::shared_ptr<Track>& track, int slotIndex, juce::Component* anchor)
{
    auto* slot = track->getSlot (slotIndex);
    if (slot == nullptr)
        return;

    juce::PopupMenu menu;
    menu.addItem (1, "Открыть окно плагина"_ru);
    menu.addItem (2, "Обход (bypass)"_ru, true, slot->bypass.load());
    menu.addSeparator();
    menu.addItem (3, "Удалить из цепочки"_ru);

    juce::Component::SafePointer<MainComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor),
                        [safe, track, slotIndex] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;

                            if (result == 1)
                            {
                                safe->openPluginEditor (track, slotIndex);
                            }
                            else if (result == 2)
                            {
                                if (auto* s = track->getSlot (slotIndex))
                                {
                                    s->bypass = ! s->bypass.load();
                                    track->refreshLatency();
                                    safe->rebuildTracks();
                                }
                            }
                            else if (result == 3)
                            {
                                if (auto* s = track->getSlot (slotIndex))
                                    safe->closeEditorsFor (s->plugin.get());
                                track->removePlugin (slotIndex);
                                safe->rebuildTracks();
                            }
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
    for (int i = pluginWindows.size(); --i >= 0;)
        if (pluginWindows[i]->getPlugin() == plugin)
            pluginWindows.remove (i);
}

// ---------------------------------------------------------------- dialogs

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (engine.deviceManager, 0, 64, 0, 64,
                                                             false, false, false, false);
    selector->setSize (560, 520);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Аудио (драйвер, устройство, каналы, буфер)"_ru;
    o.dialogBackgroundColour = juce::Colour (0xff20242a);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

void MainComponent::showPluginManager()
{
    auto* list = new juce::PluginListComponent (engine.pluginFormats, engine.knownPlugins,
                                                engine.getDataDirectory().getChildFile ("scan-deadman.txt"),
                                                pluginScanProps.get(), true);
    list->setSize (720, 460);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (list);
    o.dialogTitle = "Плагины VST3"_ru;
    o.dialogBackgroundColour = juce::Colour (0xff20242a);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::showAnalysisSettings()
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Темп и тональность"_ru,
                                            "Здесь будет выбор каналов-источников для автоматического определения "_ru
                                            "темпа и тональности. Эта функция появится в следующей версии.\n\n"_ru
                                            "Пока темп задаётся вручную или кнопкой Tap, и передаётся всем плагинам."_ru);
}

void MainComponent::setPinned (bool pinned)
{
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
            const double bpm = 60000.0 / avg;
            engine.setBpm (bpm);
            bpmSlider.setValue (engine.getBpm(), juce::dontSendNotification);
        }
    }
}

// ---------------------------------------------------------------- timers / listeners

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &engine.deviceManager)
    {
        for (auto* t : trackComponents)
            t->refreshChannelLists();
    }
    else if (source == &engine.knownPlugins)
    {
        engine.savePluginList();
    }
}

void MainComponent::timerCallback()
{
    for (auto* t : trackComponents)
        t->updateLive();

    if (++slowCounter >= 15)   // twice a second
    {
        slowCounter = 0;
        statusLabel.setText (juce::String::formatted ("CPU %d%%   |   %d Гц, блок %d   |   "_ru,
                                                      (int) std::round (engine.getCpuLoad() * 100.0f),
                                                      (int) engine.getSampleRate(), engine.getBlockSize())
                                 + engine.describeDeviceLatency(),
                             juce::dontSendNotification);
    }

    if (++autosaveCounter >= 30 * 30)   // every 30 s
    {
        autosaveCounter = 0;
        engine.saveProject();
    }
}
