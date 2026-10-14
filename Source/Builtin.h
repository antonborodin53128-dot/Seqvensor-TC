#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

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
