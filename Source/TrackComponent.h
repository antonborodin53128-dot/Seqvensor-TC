#pragma once

#include "Engine.h"

class LevelMeter : public juce::Component
{
public:
    void setLevels (float l, float r)
    {
        // fast attack, slow release
        left = juce::jmax (l, left * 0.85f);
        right = juce::jmax (r, right * 0.85f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff15181c));
        g.fillRoundedRectangle (area, 3.0f);

        auto drawBar = [&g] (juce::Rectangle<float> r, float level)
        {
            const float db = juce::Decibels::gainToDecibels (level, -60.0f);
            const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            auto fill = r.withWidth (r.getWidth() * norm);
            g.setColour (db > -1.0f ? juce::Colour (0xffe5484d) : db > -12.0f ? juce::Colour (0xffe2b93b) : juce::Colour (0xff2fb5a2));
            g.fillRoundedRectangle (fill, 2.0f);
        };

        auto inner = area.reduced (2.0f);
        const float half = inner.getHeight() / 2.0f;
        drawBar (inner.removeFromTop (half - 1.0f), left);
        inner.removeFromTop (2.0f);
        drawBar (inner, right);
    }

private:
    float left = 0.0f, right = 0.0f;
};

class TrackComponent : public juce::Component
{
public:
    struct Callbacks
    {
        std::function<void (std::shared_ptr<Track>)> remove;
        std::function<void (std::shared_ptr<Track>, int)> openEditor;
        std::function<void (std::shared_ptr<Track>, juce::Component*)> addPlugin;
        std::function<void (std::shared_ptr<Track>)> pickFile;
        std::function<void (std::shared_ptr<Track>, int, juce::Component*)> pluginMenu;
    };

    static constexpr int preferredHeight = 142;

    TrackComponent (Engine& engineToUse, std::shared_ptr<Track> trackToUse, Callbacks callbacks);

    void paint (juce::Graphics&) override;
    void resized() override;

    void rebuildPluginButtons();
    void refreshChannelLists();
    void updateLive();

    std::shared_ptr<Track> getTrack() const { return track; }

private:
    Engine& engine;
    std::shared_ptr<Track> track;
    Callbacks cb;

    juce::Label nameLabel;
    juce::ComboBox inLCombo, inRCombo, outCombo;
    juce::TextButton fileButton { "Выбрать файл…"_ru };
    juce::TextButton playButton { "▶"_ru }, stopButton { "■"_ru };
    juce::ToggleButton loopButton { "Повтор"_ru };
    juce::Slider positionSlider;
    juce::Slider gainSlider;
    juce::ToggleButton muteButton { "Mute" };
    juce::ToggleButton compButton { "Компенсировать задержку"_ru };
    juce::TextButton removeButton { "✕"_ru };
    juce::TextButton addPluginButton { "+ эффект"_ru };
    juce::Label latencyLabel;
    LevelMeter meter;

    juce::OwnedArray<juce::TextButton> pluginButtons;
    juce::OwnedArray<juce::TextButton> pluginMenuButtons;

    bool draggingPosition = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackComponent)
};
