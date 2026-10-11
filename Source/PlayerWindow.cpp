#include "PlayerWindow.h"

namespace
{
    class WaveView : public juce::Component, private juce::Timer
    {
    public:
        WaveView (Engine& e, std::shared_ptr<Track> t)
            : engine (e), track (std::move (t)), thumbnail (512, e.audioFormats, cache)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            if (track->getFile() != juce::File())
                setFile (track->getFile());
            startTimerHz (30);
        }

        void setFile (const juce::File& f)
        {
            thumbnail.setSource (new juce::FileInputSource (f));
            repaint();
        }

        bool snapToGrid = false;

        void paint (juce::Graphics& g) override
        {
            auto bounds = getLocalBounds();
            g.setColour (juce::Colour (0xff14171b));
            g.fillRect (bounds);

            const double beat = 60.0 / juce::jlimit (20.0, 400.0, engine.getBpm());
            const float w = (float) getWidth();
            const float pxPerSec = w / (float) viewSpan;

            // area before the start of the grid (negative time)
            const float zeroX = timeToX (0.0);
            if (zeroX > 0.0f)
            {
                g.setColour (juce::Colour (0xff101215));
                g.fillRect (0.0f, 0.0f, juce::jmin (zeroX, w), (float) getHeight());
            }

            // waveform
            const double len = thumbnail.getTotalLength();
            const double offset = track->getClipOffset();
            if (len > 0.0)
            {
                const double vis0 = juce::jmax (offset, viewStart);
                const double vis1 = juce::jmin (offset + len, viewStart + viewSpan);
                if (vis1 > vis0)
                {
                    auto r = juce::Rectangle<float> (timeToX (vis0), 14.0f, timeToX (vis1) - timeToX (vis0), (float) getHeight() - 14.0f);
                    g.setColour (juce::Colour (0xff1d3a4a));
                    g.fillRect (r);
                    g.setColour (juce::Colour (0xff4fc3ff));
                    thumbnail.drawChannels (g, r.toNearestInt(), vis0 - offset, vis1 - offset, 1.0f);
                }
                g.setColour (juce::Colour (0xff4fc3ff).withAlpha (0.6f));
                g.drawVerticalLine ((int) timeToX (offset), 14.0f, (float) getHeight());
            }

            // grid: beats and bars (1 bar = 4 beats)
            const double beatPx = beat * pxPerSec;
            const double barPx = beatPx * 4.0;
            const int firstBeat = (int) std::floor (viewStart / beat);
            const int lastBeat = (int) std::ceil ((viewStart + viewSpan) / beat);
            g.setFont (juce::FontOptions (11.0f));
            for (int k = firstBeat; k <= lastBeat; ++k)
            {
                const bool bar = (k % 4) == 0;
                if (! bar && beatPx < 8.0)
                    continue;
                if (bar && barPx < 8.0 && (k / 4) % juce::jmax (1, (int) std::ceil (24.0 / juce::jmax (1.0, barPx))) != 0)
                    continue;

                const float x = timeToX ((double) k * beat);
                g.setColour (bar ? juce::Colour (0x66ffffff) : juce::Colour (0x26ffffff));
                g.drawVerticalLine ((int) x, bar ? 0.0f : 14.0f, (float) getHeight());

                if (bar && k >= 0 && barPx > 26.0)
                {
                    g.setColour (juce::Colour (0xffc7ccd3));
                    g.drawText (juce::String (k / 4 + 1), (int) x + 3, 0, 40, 14, juce::Justification::left);
                }
            }

            // start of the music (grid zero)
            g.setColour (juce::Colour (0xff2fb5a2));
            g.drawVerticalLine ((int) zeroX, 0.0f, (float) getHeight());

            // playhead
            if (track->isPlaying() && len > 0.0)
            {
                const float x = timeToX (track->getPositionSeconds() + offset);
                g.setColour (juce::Colour (0xffffd24d));
                g.drawVerticalLine ((int) x, 0.0f, (float) getHeight());
            }

            if (len <= 0.0)
            {
                g.setColour (juce::Colours::grey);
                g.setFont (juce::FontOptions (16.0f));
                g.drawText ("Нажмите \"Добавить аудио\""_ru, bounds, juce::Justification::centred);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragOffsetStart = track->getClipOffset();
            dragViewStart = viewStart;
            panning = e.mods.isRightButtonDown() || e.mods.isMiddleButtonDown();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const double dt = (double) e.getDistanceFromDragStartX() / (double) juce::jmax (1, getWidth()) * viewSpan;
            if (panning)
            {
                viewStart = dragViewStart - dt;
            }
            else
            {
                double value = dragOffsetStart + dt;
                if (snapToGrid || e.mods.isShiftDown())
                {
                    const double step = 60.0 / juce::jlimit (20.0, 400.0, engine.getBpm()) / 4.0;
                    value = std::round (value / step) * step;
                }
                track->setClipOffset (value);
            }
            repaint();
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            if (wheel.deltaY == 0.0f)
                return;

            const double tAtMouse = xToTime ((float) e.x);
            const double factor = wheel.deltaY > 0.0f ? 0.8 : 1.25;   // wheel up = zoom in
            viewSpan = juce::jlimit (0.5, 900.0, viewSpan * factor);
            viewStart = tAtMouse - ((double) e.x / (double) juce::jmax (1, getWidth())) * viewSpan;
            repaint();
        }

    private:
        void timerCallback() override { repaint(); }

        float timeToX (double t) const { return (float) ((t - viewStart) / viewSpan * getWidth()); }
        double xToTime (float x) const { return viewStart + (double) x / (double) juce::jmax (1, getWidth()) * viewSpan; }

        Engine& engine;
        std::shared_ptr<Track> track;
        juce::AudioThumbnailCache cache { 4 };
        juce::AudioThumbnail thumbnail;

        double viewStart = -0.5, viewSpan = 12.0;
        double dragOffsetStart = 0.0, dragViewStart = 0.0;
        bool panning = false;
    };

    class PlayerPanel : public juce::Component, private juce::Timer
    {
    public:
        PlayerPanel (Engine& e, std::shared_ptr<Track> t)
            : engine (e), track (t), wave (e, t)
        {
            addButton.setButtonText ("Добавить аудио..."_ru);
            addButton.onClick = [this] { chooseFile(); };
            addAndMakeVisible (addButton);

            playButton.setTooltip ("Играть этот плеер"_ru);
            playButton.onClick = [this] { track->play(); };
            stopButton.setTooltip ("Стоп"_ru);
            stopButton.onClick = [this] { track->stopAndRewind(); };
            addAndMakeVisible (playButton);
            addAndMakeVisible (stopButton);

            loopButton.setToggleState (track->isLooping(), juce::dontSendNotification);
            loopButton.onClick = [this] { track->setLooping (loopButton.getToggleState()); };
            addAndMakeVisible (loopButton);

            snapButton.setToggleState (false, juce::dontSendNotification);
            snapButton.onClick = [this] { wave.snapToGrid = snapButton.getToggleState(); };
            addAndMakeVisible (snapButton);

            resetButton.setButtonText ("Сбросить сдвиг"_ru);
            resetButton.onClick = [this] { track->setClipOffset (0.0); };
            addAndMakeVisible (resetButton);

            fileLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            addAndMakeVisible (fileLabel);
            offsetLabel.setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (offsetLabel);

            hint.setText ("Колесо мыши - масштаб. Перетаскивание - сдвиг аудио по сетке метронома (Shift - по долям). "_ru
                          "Правая кнопка - прокрутка. Зелёная линия - начало музыки (5-й удар после отсчёта)."_ru,
                          juce::dontSendNotification);
            hint.setColour (juce::Label::textColourId, juce::Colours::grey);
            hint.setFont (juce::FontOptions (12.0f));
            addAndMakeVisible (hint);

            addAndMakeVisible (wave);
            startTimerHz (10);
            timerCallback();
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (10);
            auto top = r.removeFromTop (30);
            addButton.setBounds (top.removeFromLeft (170));
            top.removeFromLeft (8);
            playButton.setBounds (top.removeFromLeft (40));
            top.removeFromLeft (4);
            stopButton.setBounds (top.removeFromLeft (40));
            top.removeFromLeft (10);
            loopButton.setBounds (top.removeFromLeft (90));
            snapButton.setBounds (top.removeFromLeft (190));
            resetButton.setBounds (top.removeFromRight (150));
            top.removeFromRight (6);
            offsetLabel.setBounds (top.removeFromRight (150));

            r.removeFromTop (4);
            fileLabel.setBounds (r.removeFromTop (20));
            r.removeFromTop (4);
            hint.setBounds (r.removeFromBottom (34));
            r.removeFromBottom (4);
            wave.setBounds (r);
        }

    private:
        void chooseFile()
        {
            chooser = std::make_unique<juce::FileChooser> ("Выберите аудиофайл"_ru, juce::File(),
                                                           "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
            juce::Component::SafePointer<PlayerPanel> safe (this);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [safe] (const juce::FileChooser& fc)
                                  {
                                      if (safe == nullptr)
                                          return;
                                      const auto f = fc.getResult();
                                      if (f == juce::File())
                                          return;
                                      if (safe->track->loadFile (f))
                                          safe->wave.setFile (f);
                                      else
                                          juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "TC",
                                                                                  "Не удалось открыть файл:\n"_ru + f.getFullPathName());
                                  });
        }

        void timerCallback() override
        {
            const auto f = track->getFile();
            fileLabel.setText (f != juce::File() ? f.getFileName() + "   ("_ru + juce::String (track->getLengthSeconds(), 1) + " с)"_ru
                                                 : "Файл не выбран"_ru,
                               juce::dontSendNotification);

            const int ms = (int) std::lround (track->getClipOffset() * 1000.0);
            offsetLabel.setText ("Сдвиг: "_ru + (ms > 0 ? "+" : "") + juce::String (ms) + " мс"_ru, juce::dontSendNotification);
            playButton.setActive (track->isPlaying());
        }

        Engine& engine;
        std::shared_ptr<Track> track;
        juce::TextButton addButton, resetButton;
        IconButton playButton { IconButton::Icon::Play }, stopButton { IconButton::Icon::Stop };
        juce::ToggleButton loopButton { "Повтор"_ru };
        juce::ToggleButton snapButton { "Привязка к сетке (1/16 доли)"_ru };
        juce::Label fileLabel, offsetLabel, hint;
        WaveView wave;
        std::unique_ptr<juce::FileChooser> chooser;
    };
}

PlayerWindow::PlayerWindow (Engine& engine, std::shared_ptr<Track> trackToUse, std::function<void (PlayerWindow*)> onCloseRequested)
    : juce::DocumentWindow ("TC Плеер - "_ru + trackToUse->name, juce::Colour (0xff20242a), juce::DocumentWindow::closeButton),
      track (std::move (trackToUse)),
      onClose (std::move (onCloseRequested))
{
    setUsingNativeTitleBar (true);
    setContentOwned (new PlayerPanel (engine, track), false);
    setResizable (true, false);
    setResizeLimits (560, 260, 4000, 1200);
    setAlwaysOnTop (true);
    centreWithSize (920, 380);
    setVisible (true);
}
