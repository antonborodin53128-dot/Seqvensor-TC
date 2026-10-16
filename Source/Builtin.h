#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <vector>

// What the project tells the built-in effects (written by the main window, read by the effects).
namespace tcshared
{
    struct Project
    {
        std::atomic<int> keyTonic { -1 };       // key of the project: 0 = C ... 11 = B, -1 = none known
        std::atomic<bool> keyMinor { false };
    };
    inline Project& project() { static Project p; return p; }

    // The guitarists: every Amp plugin of the project belongs to one of four guitarists and has four scenes (presets)
    // that can be switched from the phone. Message thread only.
    struct Guitarist
    {
        int slot = 0;                  // 0..3
        juce::String name;
        int scene = 0;                 // active scene 0..3
        juce::StringArray sceneNames;  // four names
    };
    std::vector<Guitarist> guitarists();                  // one entry per slot that has an Amp
    bool setGuitarScene (int slot, int scene);            // false: no Amp for that slot
}

// Built-in effects of TC: they live inside the exe, run in the same audio callback as the track and add no latency
// of their own (the auto-tune is the only one that has to delay the sound a little, and it reports it).
// They are registered as a plugin format called "TC", so the slots of a track treat them like any other plugin.
class BuiltinFormat : public juce::AudioPluginFormat
{
public:
    static juce::String formatName() { return "TC"; }

    // one description per built-in effect
    static std::vector<juce::PluginDescription> allDescriptions();

    juce::String getName() const override { return formatName(); }
    bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override;
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& fileOrIdentifier) override;
    bool doesPluginStillExist (const juce::PluginDescription& d) override { return d.pluginFormatName == formatName(); }
    juce::String getNameOfPluginFromIdentifier (const juce::String& id) override { return id; }
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override { return true; }

protected:
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }
    void createPluginInstance (const juce::PluginDescription&, double, int, PluginCreationCallback) override;
};
