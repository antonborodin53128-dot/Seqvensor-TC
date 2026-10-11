#pragma once

#include "Engine.h"
#include "TrackComponent.h"
#include "PlayerWindow.h"

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
    // top bar / menus
    void showMainMenu();
    void showAudioSettings();
    void showPluginChooser();
    void showPluginScanner();
    void showAnalysisSettings();

    // strip menus
    void showInputMenu (const std::shared_ptr<Track>& track, juce::Component* anchor);
    void showSlotMenu (const std::shared_ptr<Track>& track, int slotIndex, juce::Component* anchor);
    void showOutputMenu (const std::shared_ptr<Track>& track, juce::Component* anchor);
    void loadPluginIntoSlot (const std::shared_ptr<Track>& track, int slotIndex, const juce::PluginDescription& desc);
    void openPluginEditor (const std::shared_ptr<Track>& track, int slotIndex);
    void closeEditorsFor (juce::AudioPluginInstance* plugin);
    void openPlayerWindow (const std::shared_ptr<Track>& track);
    void closePlayerWindowsFor (const Track* track);
    void addTrack();
    void tapTempo();
    void setPinned (bool pinned);

    void rebuildTracks();
    void refreshStrips();
    void layoutTracks();
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    juce::LookAndFeel_V4 lookAndFeel;   // must outlive every child component
    Engine engine;
    juce::TooltipWindow tooltipWindow { this, 600 };

    // top bar
    IconButton menuButton { IconButton::Icon::Menu };
    juce::Slider bpmSlider;
    juce::TextButton tapButton { "Tap" };
    juce::Label keyLabel { {}, "Тон: -"_ru };
    IconButton analysisButton { IconButton::Icon::Gear };
    juce::Label cpuLabel;
    IconButton playAllButton { IconButton::Icon::Play }, stopAllButton { IconButton::Icon::Stop };

    // tracks
    juce::Viewport viewport;
    juce::Component trackList;
    juce::OwnedArray<TrackComponent> trackComponents;
    juce::TextButton addTrackButton { "+" };
    juce::Label emptyHint;

    juce::OwnedArray<PluginWindow> pluginWindows;
    juce::OwnedArray<PlayerWindow> playerWindows;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::PropertiesFile> pluginScanProps;
    juce::Array<juce::int64> tapTimes;
    bool pinned = true;
    int autosaveCounter = 0;
    int slowCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
