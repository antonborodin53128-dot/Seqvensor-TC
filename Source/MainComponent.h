#pragma once

#include "Engine.h"
#include "TrackComponent.h"

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
    // actions
    void showAudioSettings();
    void showPluginManager();
    void showAnalysisSettings();
    void showPluginMenu (const std::shared_ptr<Track>& track, juce::Component* anchor);
    void showSlotMenu (const std::shared_ptr<Track>& track, int slotIndex, juce::Component* anchor);
    void openPluginEditor (const std::shared_ptr<Track>& track, int slotIndex);
    void closeEditorsFor (juce::AudioPluginInstance* plugin);
    void pickPlayerFile (const std::shared_ptr<Track>& track);
    void addPlayerTrack();
    void addLiveTrack();
    void tapTempo();
    void setPinned (bool pinned);

    void rebuildTracks();
    void layoutTracks();
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    juce::LookAndFeel_V4 lookAndFeel;   // must outlive every child component
    Engine engine;

    // header
    juce::Label titleLabel { {}, "TC" };
    juce::TextButton audioButton { "Аудио" }, pluginsButton { "Плагины" };
    juce::ToggleButton pinButton { "Поверх всех окон" };
    juce::Label statusLabel;

    // project panel
    juce::Label bpmCaption { {}, "ТЕМП" }, keyCaption { {}, "ТОНАЛЬНОСТЬ" };
    juce::Slider bpmSlider;
    juce::TextButton tapButton { "Tap" };
    juce::Label keyValue { {}, "—" };
    juce::TextButton analysisButton { "⚙" };

    // tracks
    juce::Viewport viewport;
    juce::Component trackList;
    juce::OwnedArray<TrackComponent> trackComponents;
    juce::TextButton addLiveButton { "+ Живой вход (вокал, инструмент)" };
    juce::TextButton addPlayerButton { "+ Плейбэк (файл)" };
    juce::Label emptyHint;

    juce::OwnedArray<PluginWindow> pluginWindows;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::PropertiesFile> pluginScanProps;
    juce::Array<juce::int64> tapTimes;
    int autosaveCounter = 0;
    int slowCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
