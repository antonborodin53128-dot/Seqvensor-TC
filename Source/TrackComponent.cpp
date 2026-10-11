#include "TrackComponent.h"

TrackComponent::TrackComponent (Engine& engineToUse, std::shared_ptr<Track> trackToUse, Callbacks callbacks)
    : engine (engineToUse), track (std::move (trackToUse)), cb (std::move (callbacks))
{
    nameLabel.setText (track->name, juce::dontSendNotification);
    nameLabel.setEditable (false, true, false);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    nameLabel.setMinimumHorizontalScale (0.6f);
    nameLabel.onTextChange = [this] { track->name = nameLabel.getText(); };
    addAndMakeVisible (nameLabel);

    removeButton.setTooltip ("Удалить дорожку"_ru);
    removeButton.onClick = [this] { if (cb.remove) cb.remove (track); };
    addAndMakeVisible (removeButton);

    inputButton.onClick = [this] { if (cb.inputMenu) cb.inputMenu (track, &inputButton); };
    addAndMakeVisible (inputButton);

    for (int i = 0; i < Track::numSlots; ++i)
    {
        slotButtons[i].onClick = [this, i] { if (cb.slotMenu) cb.slotMenu (track, i, &slotButtons[i]); };
        addAndMakeVisible (slotButtons[i]);
    }

    // player panel
    fileButton.onClick = [this] { if (cb.pickFile) cb.pickFile (track); };
    playButton.onClick = [this]
    {
        if (track->isPlaying())
            track->pause();
        else
            track->play();
    };
    stopButton.onClick = [this] { track->stopAndRewind(); };
    metroButton.setToggleState (track->metronomeOn.load(), juce::dontSendNotification);
    metroButton.onClick = [this] { track->metronomeOn = metroButton.getToggleState(); };
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

    for (juce::Component* c : { (juce::Component*) &fileButton, (juce::Component*) &playButton,
                                (juce::Component*) &stopButton, (juce::Component*) &metroButton,
                                (juce::Component*) &loopButton, (juce::Component*) &positionSlider })
        addChildComponent (c);

    muteButton.setToggleState (track->muted.load(), juce::dontSendNotification);
    muteButton.onClick = [this] { track->muted = muteButton.getToggleState(); };
    addAndMakeVisible (muteButton);

    addAndMakeVisible (readout);

    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    fader.setRange (-60.0, 12.0, 0.1);
    fader.setSkewFactorFromMidPoint (-12.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setValue (juce::Decibels::gainToDecibels (track->gainLinear.load(), -60.0f), juce::dontSendNotification);
    fader.onValueChange = [this]
    {
        const float db = (float) fader.getValue();
        track->gainLinear = db <= -59.9f ? 0.0f : juce::Decibels::decibelsToGain (db);
        fader.setTooltip (juce::String (db, 1) + " дБ"_ru);
    };
    addAndMakeVisible (fader);
    addAndMakeVisible (meter);

    recButton.setToggleState (track->recArmed.load(), juce::dontSendNotification);
    recButton.setTooltip ("Готовность к записи дорожки"_ru);
    recButton.onClick = [this] { track->recArmed = recButton.getToggleState(); };
    addAndMakeVisible (recButton);

    outputButton.onClick = [this] { if (cb.outputMenu) cb.outputMenu (track, &outputButton); };
    addAndMakeVisible (outputButton);

    compButton.setToggleState (track->compensate.load(), juce::dontSendNotification);
    compButton.setTooltip ("Выровнять эту дорожку по задержке самой медленной цепочки. "_ru
                           "Выключено - дорожка идёт без дополнительной задержки."_ru);
    compButton.onClick = [this] { track->compensate = compButton.getToggleState(); };
    addAndMakeVisible (compButton);

    refreshAll();
}

void TrackComponent::refreshAll()
{
    nameLabel.setText (track->name, juce::dontSendNotification);

    // input
    showPlayer = track->isPlayerActive();
    const int il = track->inputL.load();
    const int ir = track->inputR.load();
    const int bus = track->busId.load();
    juce::String input;
    if (showPlayer)
        input = "Вход: TC Плеер"_ru;
    else if (bus >= 0)
    {
        auto src = engine.findTrack (bus);
        input = "Bus: "_ru + (src != nullptr ? src->name : juce::String ("?"));
    }
    else if (il < 0)
        input = "Вход: нет"_ru;
    else if (ir >= 0 && ir != il)
        input = "Вход: "_ru + juce::String (il + 1) + "+" + juce::String (ir + 1);
    else
        input = "Вход: "_ru + juce::String (il + 1);
    inputButton.setButtonText (input);
    inputButton.setEnabled (! showPlayer);
    inputButton.setColour (juce::TextButton::buttonColourId, bus >= 0 ? juce::Colour (0xff6a4fa3) : juce::Colour (0xff2c3138));

    // slots
    for (int i = 0; i < Track::numSlots; ++i)
    {
        auto& b = slotButtons[i];
        auto* slot = track->getSlot (i);
        if (slot == nullptr)
        {
            b.setButtonText (juce::String (i + 1) + ". -"_ru);
            b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff23272c));
            b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff7d838c));
            b.setTooltip ("Пустой слот: выберите плагин"_ru);
        }
        else
        {
            const juce::String n = slot->builtinPlayer ? "TC Плеер"_ru : slot->description.name;
            b.setButtonText (n);
            juce::Colour base = slot->builtinPlayer ? juce::Colour (0xff3a5fcd) : juce::Colour (0xff2b6f66);
            b.setColour (juce::TextButton::buttonColourId, slot->bypass.load() ? juce::Colour (0xff3a3d42) : base);
            b.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            b.setTooltip (slot->builtinPlayer ? n : slot->description.manufacturerName + " - " + n);
        }
    }

    // output
    const int pair = track->outputPair.load();
    outputButton.setButtonText (pair < 0 ? "Выход: нет"_ru
                                         : "Выход: "_ru + juce::String (pair * 2 + 1) + "-" + juce::String (pair * 2 + 2));

    for (juce::Component* c : { (juce::Component*) &fileButton, (juce::Component*) &playButton,
                                (juce::Component*) &stopButton, (juce::Component*) &metroButton,
                                (juce::Component*) &loopButton, (juce::Component*) &positionSlider })
        c->setVisible (showPlayer);

    const auto f = track->getFile();
    fileButton.setButtonText (f != juce::File() ? f.getFileName() : "Выбрать файл..."_ru);

    resized();
    repaint();
}

void TrackComponent::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (juce::Colour (0xff20242a));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (juce::Colour (0xff2fb5a2));
    g.fillRoundedRectangle (r.withHeight (4.0f), 2.0f);
}

void TrackComponent::resized()
{
    auto area = getLocalBounds().reduced (8, 8);
    area.removeFromTop (4);

    auto nameRow = area.removeFromTop (34);
    removeButton.setBounds (nameRow.removeFromRight (22).withSizeKeepingCentre (22, 22));
    nameLabel.setBounds (nameRow);

    area.removeFromTop (4);
    inputButton.setBounds (area.removeFromTop (28));
    area.removeFromTop (6);

    for (int i = 0; i < Track::numSlots; ++i)
    {
        slotButtons[i].setBounds (area.removeFromTop (26));
        area.removeFromTop (3);
    }

    if (showPlayer)
    {
        area.removeFromTop (3);
        fileButton.setBounds (area.removeFromTop (24));
        area.removeFromTop (3);
        auto row = area.removeFromTop (26);
        playButton.setBounds (row.removeFromLeft (36));
        row.removeFromLeft (4);
        stopButton.setBounds (row.removeFromLeft (36));
        row.removeFromLeft (4);
        loopButton.setBounds (row);
        area.removeFromTop (2);
        metroButton.setBounds (area.removeFromTop (22));
        positionSlider.setBounds (area.removeFromTop (18));
    }

    area.removeFromTop (6);
    muteButton.setBounds (area.removeFromTop (30));
    area.removeFromTop (4);

    auto bottom = area.removeFromBottom (22);
    compButton.setBounds (bottom);
    area.removeFromBottom (4);
    outputButton.setBounds (area.removeFromBottom (28));
    area.removeFromBottom (4);
    recButton.setBounds (area.removeFromBottom (30));
    area.removeFromBottom (4);

    readout.setBounds (area.removeFromTop (22));
    area.removeFromTop (3);
    fader.setBounds (area.removeFromLeft (40));
    area.removeFromLeft (4);
    meter.setBounds (area);
}

void TrackComponent::updateLive()
{
    const float pl = track->peakL.exchange (0.0f);
    const float pr = track->peakR.exchange (0.0f);
    meter.setLevels (pl, pr);
    readout.update (juce::jmax (pl, pr));

    const double sr = juce::jmax (1.0, engine.getSampleRate());
    const double ms = track->getCachedLatencySamples() * 1000.0 / sr;
    compButton.setButtonText ("Комп. "_ru + juce::String (ms, 1) + " мс"_ru);

    if (track->isPlayerActive() != showPlayer)
        refreshAll();

    if (showPlayer)
    {
        const double len = track->getLengthSeconds();
        positionSlider.setRange (0.0, juce::jmax (0.001, len), 0.0);
        if (! draggingPosition)
            positionSlider.setValue (track->getPositionSeconds(), juce::dontSendNotification);

        playButton.setIcon (track->isPlaying() ? IconButton::Icon::Pause : IconButton::Icon::Play);
    }
}
