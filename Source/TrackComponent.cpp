#include "TrackComponent.h"

namespace
{
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

TrackComponent::TrackComponent (Engine& engineToUse, std::shared_ptr<Track> trackToUse, Callbacks callbacks)
    : engine (engineToUse), track (std::move (trackToUse)), cb (std::move (callbacks))
{
    const bool isLive = track->kind == Track::Kind::Live;

    nameLabel.setText (track->name, juce::dontSendNotification);
    nameLabel.setEditable (false, true, false);
    nameLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    nameLabel.onTextChange = [this] { track->name = nameLabel.getText(); };
    addAndMakeVisible (nameLabel);

    if (isLive)
    {
        addAndMakeVisible (inLCombo);
        addAndMakeVisible (inRCombo);
        inLCombo.onChange = [this] { track->inputL = inLCombo.getSelectedId() - 2; };
        inRCombo.onChange = [this] { track->inputR = inRCombo.getSelectedId() - 2; };
    }
    else
    {
        addAndMakeVisible (fileButton);
        addAndMakeVisible (playButton);
        addAndMakeVisible (stopButton);
        addAndMakeVisible (loopButton);
        addAndMakeVisible (positionSlider);

        fileButton.onClick = [this] { if (cb.pickFile) cb.pickFile (track); };
        playButton.onClick = [this]
        {
            if (track->isPlaying())
                track->pause();
            else
                track->play();
        };
        stopButton.onClick = [this] { track->stopAndRewind(); };
        loopButton.setToggleState (track->isLooping(), juce::dontSendNotification);
        loopButton.onClick = [this] { track->setLooping (loopButton.getToggleState()); };

        positionSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        positionSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        positionSlider.setRange (0.0, 1.0, 0.0);
        positionSlider.onDragStart = [this] { draggingPosition = true; };
        positionSlider.onDragEnd = [this] { draggingPosition = false; };
        positionSlider.onValueChange = [this]
        {
            if (draggingPosition)
                track->setPositionSeconds (positionSlider.getValue());
        };
    }

    addAndMakeVisible (outCombo);
    outCombo.onChange = [this] { track->outputPair = outCombo.getSelectedId() - 1; };

    gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
    gainSlider.setRange (-60.0, 12.0, 0.1);
    gainSlider.setSkewFactorFromMidPoint (-12.0);
    gainSlider.setTextValueSuffix (" дБ"_ru);
    gainSlider.setDoubleClickReturnValue (true, 0.0);
    gainSlider.setValue (juce::Decibels::gainToDecibels (track->gainLinear.load(), -60.0f), juce::dontSendNotification);
    gainSlider.onValueChange = [this]
    {
        track->gainLinear = juce::Decibels::decibelsToGain ((float) gainSlider.getValue(), -60.0f);
    };
    addAndMakeVisible (gainSlider);

    muteButton.setToggleState (track->muted.load(), juce::dontSendNotification);
    muteButton.onClick = [this] { track->muted = muteButton.getToggleState(); };
    addAndMakeVisible (muteButton);

    compButton.setToggleState (track->compensate.load(), juce::dontSendNotification);
    compButton.setTooltip ("Выровнять эту дорожку по задержке самой «медленной» цепочки. Выключено = дорожка идёт без дополнительной задержки."_ru);
    compButton.onClick = [this] { track->compensate = compButton.getToggleState(); };
    addAndMakeVisible (compButton);

    removeButton.onClick = [this] { if (cb.remove) cb.remove (track); };
    addAndMakeVisible (removeButton);

    addPluginButton.onClick = [this] { if (cb.addPlugin) cb.addPlugin (track, &addPluginButton); };
    addAndMakeVisible (addPluginButton);

    latencyLabel.setJustificationType (juce::Justification::centredRight);
    latencyLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (latencyLabel);
    addAndMakeVisible (meter);

    refreshChannelLists();
    rebuildPluginButtons();
}

void TrackComponent::refreshChannelLists()
{
    auto* device = engine.deviceManager.getCurrentAudioDevice();

    if (track->kind == Track::Kind::Live)
    {
        const auto ins = activeChannelNames (device, true);

        inLCombo.clear (juce::dontSendNotification);
        inRCombo.clear (juce::dontSendNotification);
        inLCombo.addItem ("Вход: —"_ru, 1);
        inRCombo.addItem ("R: моно"_ru, 1);
        for (int i = 0; i < ins.size(); ++i)
        {
            inLCombo.addItem ("Вход: "_ru + ins[i], i + 2);
            inRCombo.addItem ("R: " + ins[i], i + 2);
        }
        inLCombo.setSelectedId (track->inputL.load() + 2, juce::dontSendNotification);
        inRCombo.setSelectedId (track->inputR.load() + 2, juce::dontSendNotification);
    }

    const auto outs = activeChannelNames (device, false);
    outCombo.clear (juce::dontSendNotification);
    const int pairs = juce::jmax (1, (outs.size() + 1) / 2);
    for (int p = 0; p < pairs; ++p)
    {
        juce::String label = "Выход: "_ru;
        if (p * 2 < outs.size())
            label += outs[p * 2];
        if (p * 2 + 1 < outs.size())
            label += " / " + outs[p * 2 + 1];
        if (outs.isEmpty())
            label += juce::String (p * 2 + 1) + "-" + juce::String (p * 2 + 2);
        outCombo.addItem (label, p + 1);
    }
    outCombo.setSelectedId (track->outputPair.load() + 1, juce::dontSendNotification);
}

void TrackComponent::rebuildPluginButtons()
{
    pluginButtons.clear();
    pluginMenuButtons.clear();

    for (int i = 0; i < track->getNumPlugins(); ++i)
    {
        auto* slot = track->getSlot (i);
        if (slot == nullptr)
            continue;

        auto* b = pluginButtons.add (new juce::TextButton (slot->description.name));
        b->setTooltip (slot->description.manufacturerName + " — "_ru + slot->description.name);
        b->setColour (juce::TextButton::buttonColourId, slot->bypass.load() ? juce::Colour (0xff3a3d42) : juce::Colour (0xff2b6f66));
        b->onClick = [this, i] { if (cb.openEditor) cb.openEditor (track, i); };
        addAndMakeVisible (b);

        auto* m = pluginMenuButtons.add (new juce::TextButton ("⋯"_ru));
        m->onClick = [this, i, m] { if (cb.pluginMenu) cb.pluginMenu (track, i, m); };
        addAndMakeVisible (m);
    }

    resized();
    repaint();
}

void TrackComponent::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    g.setColour (juce::Colour (0xff20242a));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (track->kind == Track::Kind::Live ? juce::Colour (0xff2fb5a2) : juce::Colour (0xff6c8cff));
    g.fillRoundedRectangle (r.withWidth (4.0f), 2.0f);
}

void TrackComponent::resized()
{
    auto area = getLocalBounds().reduced (12, 8);
    area.removeFromLeft (4);

    // row 1
    auto row1 = area.removeFromTop (28);
    removeButton.setBounds (row1.removeFromRight (30));
    row1.removeFromRight (8);
    outCombo.setBounds (row1.removeFromRight (240));
    row1.removeFromRight (8);
    nameLabel.setBounds (row1.removeFromLeft (150));
    row1.removeFromLeft (8);

    if (track->kind == Track::Kind::Live)
    {
        inLCombo.setBounds (row1.removeFromLeft (200));
        row1.removeFromLeft (8);
        inRCombo.setBounds (row1.removeFromLeft (170));
    }
    else
    {
        fileButton.setBounds (row1.removeFromLeft (160));
        row1.removeFromLeft (6);
        playButton.setBounds (row1.removeFromLeft (40));
        row1.removeFromLeft (4);
        stopButton.setBounds (row1.removeFromLeft (40));
        row1.removeFromLeft (6);
        loopButton.setBounds (row1.removeFromLeft (80));
        positionSlider.setBounds (row1);
    }

    area.removeFromTop (6);

    // row 2
    auto row2 = area.removeFromTop (28);
    gainSlider.setBounds (row2.removeFromLeft (260));
    row2.removeFromLeft (8);
    muteButton.setBounds (row2.removeFromLeft (70));
    compButton.setBounds (row2.removeFromLeft (210));
    latencyLabel.setBounds (row2.removeFromRight (230));
    row2.removeFromRight (8);
    meter.setBounds (row2.reduced (0, 4));

    area.removeFromTop (8);

    // row 3: plugin chain
    auto row3 = area.removeFromTop (28);
    const int n = pluginButtons.size();
    const int addW = 90;
    const int menuW = 28;
    const int gap = 6;
    const int avail = row3.getWidth() - addW - gap;
    int each = n > 0 ? juce::jlimit (70, 170, avail / n - menuW - gap) : 0;

    for (int i = 0; i < n; ++i)
    {
        pluginButtons[i]->setBounds (row3.removeFromLeft (each));
        pluginMenuButtons[i]->setBounds (row3.removeFromLeft (menuW));
        row3.removeFromLeft (gap);
    }
    addPluginButton.setBounds (row3.removeFromLeft (addW));
}

void TrackComponent::updateLive()
{
    meter.setLevels (track->peakL.exchange (0.0f), track->peakR.exchange (0.0f));

    const double sr = juce::jmax (1.0, engine.getSampleRate());
    const double ms = track->getCachedLatencySamples() * 1000.0 / sr;
    latencyLabel.setText (juce::String::formatted ("задержка цепочки: %.1f мс"_ru, ms), juce::dontSendNotification);

    if (track->kind == Track::Kind::Player)
    {
        const double len = track->getLengthSeconds();
        positionSlider.setRange (0.0, juce::jmax (0.001, len), 0.0);
        if (! draggingPosition)
            positionSlider.setValue (track->getPositionSeconds(), juce::dontSendNotification);

        playButton.setButtonText (track->isPlaying() ? "❚❚"_ru : "▶"_ru);
        const auto f = track->getFile();
        fileButton.setButtonText (f != juce::File() ? f.getFileName() : "Выбрать файл…"_ru);
    }
}
