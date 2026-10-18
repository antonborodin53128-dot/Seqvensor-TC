#include "TrackComponent.h"

TrackComponent::TrackComponent (Engine& engineToUse, std::shared_ptr<Track> trackToUse, TimelineView& view,
                                Callbacks callbacks, LaneComponent::Callbacks laneCallbacks)
    : engine (engineToUse), track (std::move (trackToUse)), cb (std::move (callbacks)),
      lane (engineToUse, track, view, std::move (laneCallbacks))
{
    grip.setTooltip ("Зажмите и перетащите, чтобы поменять дорожку местами"_ru);
    grip.onStart = [this]
    {
        // like dragging a plugin: a picture of the strip follows the mouse, the original stays dimmed in its place
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            auto image = createComponentSnapshot (juce::Rectangle<int> (0, 0, headerWidth - 4, getHeight()), true);
            setAlpha (0.35f);
            container->startDragging ("tctrack:" + juce::String (track->id), this, juce::ScaledImage (image), true);
        }
    };
    addAndMakeVisible (grip);

    nameLabel.setText (track->name, juce::dontSendNotification);
    nameLabel.setEditable (false, true, false);
    nameLabel.setJustificationType (juce::Justification::centredLeft);
    nameLabel.setFont (juce::FontOptions (19.0f, juce::Font::bold));
    nameLabel.setMinimumHorizontalScale (0.6f);
    nameLabel.setTooltip (track->name);   // the full name when it does not fit
    nameLabel.onTextChange = [this] { track->name = nameLabel.getText(); nameLabel.setTooltip (track->name); };
    addAndMakeVisible (nameLabel);

    removeButton.setTooltip ("Удалить дорожку"_ru);
    removeButton.onClick = [this] { if (cb.remove) cb.remove (track); };
    addAndMakeVisible (removeButton);

    inputButton.onClick = [this] { if (cb.inputMenu) cb.inputMenu (track, &inputButton); };
    addAndMakeVisible (inputButton);
    outputButton.onClick = [this] { if (cb.outputMenu) cb.outputMenu (track, &outputButton); };
    addAndMakeVisible (outputButton);

    metroButton.setColours (Pal::violet, Pal::surface, juce::Colour (0xffe3c4fa), Pal::hairline, Pal::textDim);
    metroButton.setToggleState (track->metronomeOn.load(), juce::dontSendNotification);
    metroButton.setTooltip ("Метроном на этой дорожке (в темпе проекта). Если он включён хотя бы на одной дорожке, "_ru
                            "общий Play начинается с 4 ударов отсчёта."_ru);
    metroButton.onClick = [this] { track->metronomeOn = metroButton.getToggleState(); };
    addAndMakeVisible (metroButton);

    for (int i = 0; i < Track::numSlots; ++i)
    {
        slotButtons[i].onClick = [this, i]
        {
            if (slotButtons[i].consumeDragFlag())
                return;   // the press was a drag, not a click
            if (cb.slotClick)
                cb.slotClick (track, i, &slotButtons[i]);
        };
        slotButtons[i].onRightClick = [this, i] { if (cb.slotBypass && track->getSlot (i) != nullptr) cb.slotBypass (track, i); };
        slotButtons[i].dragPayload = [this, i]() -> juce::var
        {
            if (track->getSlot (i) == nullptr)
                return {};
            return juce::var ("tcslot:" + juce::String (track->id) + ":" + juce::String (i));
        };
        slotButtons[i].onDropped = [this, i] (const juce::String& payload, bool copy)
        {
            if (cb.slotDropped)
                cb.slotDropped (track, i, payload, copy);
        };
        addAndMakeVisible (slotButtons[i]);

        slotArrows[i].setTooltip ("Меню слота: обход, заменить, убрать"_ru);
        slotArrows[i].onClick = [this, i] { if (cb.slotMenu) cb.slotMenu (track, i, &slotArrows[i]); };
        addChildComponent (slotArrows[i]);
    }

    muteButton.setToggleState (track->muted.load(), juce::dontSendNotification);
    muteButton.onClick = [this] { track->muted = muteButton.getToggleState(); };
    addAndMakeVisible (muteButton);

    soloButton.setColours (Pal::yellow.darker (0.45f), Pal::surface, juce::Colour (0xffffe9a8), Pal::hairline, Pal::textDim);
    soloButton.setToggleState (track->soloed.load(), juce::dontSendNotification);
    soloButton.setTooltip ("Соло: слышны только дорожки с включённым соло"_ru);
    soloButton.onClick = [this] { track->soloed = soloButton.getToggleState(); };
    addAndMakeVisible (soloButton);

    panSlider.setLookAndFeel (&panLook);
    panSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    panSlider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    panSlider.setColour (juce::Slider::rotarySliderFillColourId, Pal::accent);
    panSlider.setColour (juce::Slider::rotarySliderOutlineColourId, Pal::hairline);
    panSlider.setColour (juce::Slider::thumbColourId, Pal::text);
    panSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    panSlider.setRange (-1.0, 1.0, 0.01);
    panSlider.setDoubleClickReturnValue (true, 0.0);
    panSlider.setValue (track->pan.load(), juce::dontSendNotification);
    auto panTip = [this]
    {
        const int v = (int) std::round (panSlider.getValue() * 100.0);
        panSlider.setTooltip ("Панорама: "_ru + (v == 0 ? juce::String ("центр"_ru) : (v < 0 ? "L" : "R") + juce::String (std::abs (v)))
                              + " (двойной клик - в центр)"_ru);
    };
    panTip();
    panSlider.onValueChange = [this, panTip] { track->pan = (float) panSlider.getValue(); panTip(); };
    addAndMakeVisible (panSlider);

    recButton.setToggleState (track->recArmed.load(), juce::dontSendNotification);
    recButton.setTooltip ("Готовность к записи дорожки"_ru);
    recButton.onClick = [this] { track->recArmed = recButton.getToggleState(); };
    addAndMakeVisible (recButton);

    monoButton.setColours (Pal::accent.darker (0.5f), Pal::surface, Pal::accent, Pal::hairline, Pal::textDim);
    monoButton.setToggleState (track->mono.load(), juce::dontSendNotification);
    monoButton.setButtonText (track->mono.load() ? "MONO" : "ST");
    monoButton.setTooltip ("MONO / ST: дорожка в моно (один канал, меньше нагрузки, выход на один канал) или в стерео"_ru);
    monoButton.onClick = [this]
    {
        track->setMono (monoButton.getToggleState());   // a short fade while the plugins change their layout
        refreshAll();
    };
    addAndMakeVisible (monoButton);

    compButton.setToggleState (track->compensate.load(), juce::dontSendNotification);
    compButton.setTooltip ("Задержка цепочки этой дорожки применяется ко всем остальным дорожкам. У нескольких дорожек задержки суммируются."_ru);
    compButton.onClick = [this] { track->compensate = compButton.getToggleState(); };
    addAndMakeVisible (compButton);

    addAndMakeVisible (readout);

    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    fader.setRange (-60.0, 12.0, 0.1);
    fader.setSkewFactorFromMidPoint (-12.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setValue (juce::Decibels::gainToDecibels (track->gainLinear.load(), -60.0f), juce::dontSendNotification);
    readout.setFaderDb (fader.getValue());
    fader.onValueChange = [this]
    {
        const float db = (float) fader.getValue();
        track->gainLinear = db <= -59.9f ? 0.0f : juce::Decibels::decibelsToGain (db);
        fader.setTooltip (juce::String (db, 1) + " дБ"_ru);
        readout.setFaderDb ((double) db);
    };
    addAndMakeVisible (fader);
    addAndMakeVisible (meter);

    addAndMakeVisible (lane);
    addChildComponent (shield);

    refreshAll();
}

void TrackComponent::refreshAll()
{
    nameLabel.setText (track->name, juce::dontSendNotification);
    nameLabel.setTooltip (track->name);

    // input
    const int il = track->inputL.load();
    const int ir = track->inputR.load();
    const int bus = track->busId.load();
    juce::String input;
    if (bus >= 0)
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
    inputButton.setColour (juce::TextButton::buttonColourId, bus >= 0 ? juce::Colour (0xff5e5ce6) : Pal::surface);

    // slots
    for (int i = 0; i < Track::numSlots; ++i)
    {
        auto& b = slotButtons[i];
        auto* slot = track->getSlot (i);
        slotUsed[i] = slot != nullptr;

        if (slot == nullptr)
        {
            b.setButtonText (juce::String (i + 1) + ". -"_ru);
            b.setColour (juce::TextButton::buttonColourId, Pal::surface.withAlpha (0.5f));
            b.setColour (juce::TextButton::textColourOffId, Pal::textDim);
            b.setTooltip ("Пустой слот: выберите плагин"_ru);
        }
        else
        {
            b.setButtonText (slot->description.name);
            b.setColour (juce::TextButton::buttonColourId, slot->bypass.load() ? Pal::surfaceHi : Pal::accent.withMultipliedBrightness (0.5f));
            b.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            b.setTooltip ("Левая кнопка - открыть плагин, правая - bypass. "_ru + slot->description.manufacturerName + " - " + slot->description.name);
        }
        slotArrows[i].setVisible (slot != nullptr);
    }

    // output
    const bool isMono = track->mono.load();
    const int pair = track->outputPair.load();
    const int channel = isMono ? track->outputChannel.load() : -1;
    if (channel >= 0)
        outputButton.setButtonText ("Выход: "_ru + juce::String (channel + 1));
    else
        outputButton.setButtonText (pair < 0 ? "Выход: нет"_ru
                                             : "Выход: "_ru + juce::String (pair * 2 + 1) + "-" + juce::String (pair * 2 + 2));

    monoButton.setToggleState (isMono, juce::dontSendNotification);
    monoButton.setButtonText (isMono ? "MONO" : "ST");
    panSlider.setVisible (channel < 0);   // one output channel needs no pan

    resized();
    repaint();
}

void TrackComponent::paint (juce::Graphics& g)
{
    auto r = juce::Rectangle<float> (0.0f, 1.0f, (float) headerWidth - 4.0f, (float) getHeight() - 2.0f);
    g.setColour (Pal::surfaceLo);
    g.fillRoundedRectangle (r, Pal::radiusBig);
    g.setColour (Pal::hairline);
    g.drawRoundedRectangle (r.reduced (0.5f), Pal::radiusBig, 1.0f);
}

void TrackComponent::resized()
{
    auto area = getLocalBounds();
    lane.setBounds (area.withTrimmedLeft (headerWidth));
    shield.setBounds (0, 0, headerWidth, getHeight());

    auto h = juce::Rectangle<int> (0, 0, headerWidth - 4, getHeight()).reduced (10, 8);

    // column A: name, input, output, metronome
    auto colA = h.removeFromLeft (132);
    h.removeFromLeft (8);
    auto nameRow = colA.removeFromTop (28);
    removeButton.setBounds (nameRow.removeFromRight (20).withSizeKeepingCentre (20, 20));
    grip.setBounds (nameRow.removeFromLeft (16));
    nameLabel.setBounds (nameRow);
    colA.removeFromTop (4);
    inputButton.setBounds (colA.removeFromTop (26));
    colA.removeFromTop (4);
    outputButton.setBounds (colA.removeFromTop (26));
    colA.removeFromTop (4);
    metroButton.setBounds (colA.removeFromTop (26));

    // column B: five plugin slots
    auto colB = h.removeFromLeft (122);
    h.removeFromLeft (8);
    for (int i = 0; i < Track::numSlots; ++i)
    {
        auto row = colB.removeFromTop (20);
        if (slotUsed[i])
        {
            slotArrows[i].setBounds (row.removeFromRight (18));
            row.removeFromRight (2);
        }
        slotButtons[i].setBounds (row);
        colB.removeFromTop (4);
    }

    // column C: mute, rec, compensation
    auto colC = h.removeFromLeft (100);
    h.removeFromLeft (8);
    auto msRow = colC.removeFromTop (28);
    muteButton.setBounds (msRow.removeFromLeft (48));
    soloButton.setBounds (msRow.removeFromRight (48));
    colC.removeFromTop (4);
    panSlider.setBounds (colC.removeFromRight (46).removeFromTop (58));
    recButton.setBounds (colC.removeFromTop (26));
    colC.removeFromTop (4);
    compButton.setBounds (colC.removeFromTop (22));
    colC.removeFromTop (4);
    monoButton.setBounds (colC.removeFromTop (22));

    // column D: numeric peak, fader and meter
    readout.setBounds (h.removeFromTop (20));
    h.removeFromTop (3);
    fader.setBounds (h.removeFromLeft (30));
    h.removeFromLeft (3);
    meter.setBounds (h);
}

void TrackComponent::updateLive()
{
    const float pl = track->peakL.exchange (0.0f);
    const float pr = track->peakR.exchange (0.0f);
    const float rl = track->rmsL.exchange (0.0f);
    const float rr = track->rmsR.exchange (0.0f);
    meter.setLevels (pl, pr, rl, rr);

    const double sr = juce::jmax (1.0, engine.getSampleRate());
    const double ms = track->getCachedLatencySamples() * 1000.0 / sr;
    compButton.setTooltip ("Задержка цепочки: "_ru + juce::String (ms, 1) + " мс. Включено - эта задержка применяется ко всем остальным дорожкам "_ru
                           "(у нескольких дорожек задержки суммируются)."_ru);
}
