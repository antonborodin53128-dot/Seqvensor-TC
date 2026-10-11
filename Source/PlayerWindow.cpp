#include "PlayerWindow.h"

#include <juce_dsp/juce_dsp.h>

namespace
{
    // Tempo estimate from the audio file: spectral-flux onset envelope + autocorrelation.
    // Returns 0 if nothing sensible was found.
    double detectTempo (juce::AudioFormatReader& reader)
    {
        const double sr = reader.sampleRate;
        const juce::int64 total = juce::jmin (reader.lengthInSamples, (juce::int64) (sr * 150.0));
        if (sr <= 0.0 || total < (juce::int64) (sr * 8.0))
            return 0.0;

        constexpr int order = 10, fftSize = 1 << order, hop = 256, bins = fftSize / 2;
        juce::dsp::FFT fft (order);
        juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann);

        std::vector<float> mono ((size_t) total);
        {
            juce::AudioBuffer<float> buf (juce::jmax (1, (int) reader.numChannels), 65536);
            juce::int64 pos = 0;
            while (pos < total)
            {
                const int n = (int) juce::jmin ((juce::int64) 65536, total - pos);
                reader.read (&buf, 0, n, pos, true, true);
                for (int i = 0; i < n; ++i)
                {
                    float sum = 0.0f;
                    for (int c = 0; c < buf.getNumChannels(); ++c)
                        sum += buf.getSample (c, i);
                    mono[(size_t) (pos + i)] = sum / (float) buf.getNumChannels();
                }
                pos += n;
            }
        }

        const int frames = (int) ((total - fftSize) / hop);
        if (frames < 200)
            return 0.0;

        std::vector<float> env ((size_t) frames, 0.0f), prev ((size_t) bins, 0.0f), data ((size_t) fftSize * 2);
        for (int f = 0; f < frames; ++f)
        {
            std::fill (data.begin(), data.end(), 0.0f);
            std::copy (mono.begin() + (size_t) f * hop, mono.begin() + (size_t) f * hop + fftSize, data.begin());
            window.multiplyWithWindowingTable (data.data(), (size_t) fftSize);
            fft.performFrequencyOnlyForwardTransform (data.data());

            float flux = 0.0f;
            for (int k = 2; k < bins / 2; ++k)
            {
                const float m = std::log (1.0f + 100.0f * data[(size_t) k]);
                flux += juce::jmax (0.0f, m - prev[(size_t) k]);
                prev[(size_t) k] = m;
            }
            env[(size_t) f] = flux;
        }

        // remove the slow trend
        const double fps = sr / hop;
        const int w = juce::jmax (3, (int) (fps * 0.4));
        std::vector<float> flat ((size_t) frames, 0.0f);
        double run = 0.0;
        for (int i = 0; i < frames; ++i)
        {
            run += env[(size_t) i];
            if (i >= 2 * w + 1)
                run -= env[(size_t) (i - 2 * w - 1)];
            const int cnt = juce::jmin (i + 1, 2 * w + 1);
            const double mean = run / cnt;
            flat[(size_t) i] = juce::jmax (0.0f, env[(size_t) i] - (float) mean);
        }

        const int lagMin = (int) std::floor (fps * 60.0 / 200.0);
        const int lagMax = (int) std::ceil (fps * 60.0 / 60.0);
        const int lagLimit = lagMax * 2 + 2;
        if (lagLimit >= frames / 2)
            return 0.0;

        std::vector<double> ac ((size_t) lagLimit + 2, 0.0);
        for (int lag = lagMin - 1; lag <= lagLimit; ++lag)
        {
            double sum = 0.0;
            for (int i = 0; i + lag < frames; ++i)
                sum += (double) flat[(size_t) i] * flat[(size_t) (i + lag)];
            ac[(size_t) lag] = sum / (double) (frames - lag);
        }

        int bestLag = 0;
        double best = 0.0;
        for (int lag = lagMin; lag <= lagMax; ++lag)
        {
            const double bpm = 60.0 * fps / lag;
            const double prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 120.0) / 0.9, 2.0));
            const double score = prior * (ac[(size_t) lag] + 0.5 * ac[(size_t) (lag * 2)]);
            if (score > best)
            {
                best = score;
                bestLag = lag;
            }
        }
        if (bestLag <= 0)
            return 0.0;

        // parabolic refinement
        double lagF = bestLag;
        const double y0 = ac[(size_t) bestLag - 1], y1 = ac[(size_t) bestLag], y2 = ac[(size_t) bestLag + 1];
        const double den = y0 - 2.0 * y1 + y2;
        if (std::abs (den) > 1.0e-12)
            lagF += juce::jlimit (-1.0, 1.0, 0.5 * (y0 - y2) / den);

        double bpm = 60.0 * fps / lagF;
        while (bpm < 70.0) bpm *= 2.0;
        while (bpm > 180.0) bpm *= 0.5;
        return std::round (bpm * 10.0) / 10.0;
    }

    // sub-pixel vertical line, so zooming moves every grid line smoothly and evenly
    void vline (juce::Graphics& g, float x, float y, float h)
    {
        g.fillRect (x - 0.5f, y, 1.0f, h);
    }

    class WaveView : public juce::Component, private juce::Timer, private juce::ScrollBar::Listener
    {
    public:
        WaveView (Engine& e, std::shared_ptr<Track> t)
            : engine (e), track (std::move (t)), thumbnail (512, e.audioFormats, cache)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            if (track->getFile() != juce::File())
                setFile (track->getFile());

            viewStart = -4.0 * beatSeconds() - 0.3;
            scrollbar.setAutoHide (false);
            scrollbar.addListener (this);
            addAndMakeVisible (scrollbar);
            startTimerHz (30);
        }

        ~WaveView() override { scrollbar.removeListener (this); }

        void setFile (const juce::File& f)
        {
            thumbnail.setSource (new juce::FileInputSource (f));
            repaint();
        }

        bool snapToGrid = false;

        void resized() override
        {
            scrollbar.setBounds (0, getHeight() - scrollH, getWidth(), scrollH);
            updateScrollbar();
        }

        void paint (juce::Graphics& g) override
        {
            const int waveH = getHeight() - scrollH;
            g.setColour (juce::Colour (0xff14171b));
            g.fillRect (0, 0, getWidth(), waveH);

            const double beat = beatSeconds();
            const float w = (float) getWidth();
            const float pxPerSec = w / (float) viewSpan;
            const double countInStart = -4.0 * beat;

            // count-in area (4 beats before the first bar) and the part before it (never played)
            const float cx0 = timeToX (countInStart), zeroX = timeToX (0.0);
            if (cx0 > 0.0f)
            {
                g.setColour (juce::Colour (0xff0d0f12));
                g.fillRect (0.0f, 0.0f, juce::jmin (cx0, w), (float) waveH);
            }
            {
                const float a = juce::jmax (0.0f, cx0), b = juce::jmin (w, zeroX);
                if (b > a)
                {
                    g.setColour (juce::Colour (0xff1a2020));
                    g.fillRect (a, 0.0f, b - a, (float) waveH);
                    g.setColour (juce::Colour (0xff6f8f8a));
                    g.setFont (juce::FontOptions (11.0f));
                    g.drawText ("отсчёт"_ru, (int) a + 4, waveH - 16, 80, 14, juce::Justification::left);
                }
            }

            // waveform: normal after the start of the count-in, dimmed before it
            const double len = thumbnail.getTotalLength();
            const double offset = track->getClipOffset();
            if (len > 0.0)
            {
                auto drawPart = [&] (double t0, double t1, juce::Colour fill, juce::Colour wave)
                {
                    const double v0 = juce::jmax (t0, viewStart), v1 = juce::jmin (t1, viewStart + viewSpan);
                    if (v1 <= v0)
                        return;
                    auto r = juce::Rectangle<float> (timeToX (v0), 14.0f, timeToX (v1) - timeToX (v0), (float) waveH - 14.0f);
                    g.setColour (fill);
                    g.fillRect (r);
                    g.setColour (wave);
                    thumbnail.drawChannels (g, r.toNearestInt(), v0 - offset, v1 - offset, 1.0f);
                };

                drawPart (offset, juce::jmin (offset + len, countInStart), juce::Colour (0xff2a2d31), juce::Colour (0xff6a7078));
                drawPart (juce::jmax (offset, countInStart), offset + len, juce::Colour (0xff1d3a4a), juce::Colour (0xff4fc3ff));

                g.setColour (juce::Colour (0xff4fc3ff).withAlpha (0.6f));
                vline (g, timeToX (offset), 14.0f, (float) waveH);
            }

            // grid: beats and bars (1 bar = 4 beats); the count-in is shown as bar 0
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
                if (bar && barPx < 8.0 && std::abs (k / 4) % juce::jmax (1, (int) std::ceil (24.0 / juce::jmax (1.0, barPx))) != 0)
                    continue;

                const float x = timeToX ((double) k * beat);
                g.setColour (bar ? juce::Colour (0x66ffffff) : juce::Colour (0x26ffffff));
                vline (g, x, bar ? 0.0f : 14.0f, (float) waveH);

                if (bar && k >= 0 && barPx > 26.0)
                {
                    g.setColour (juce::Colour (0xffc7ccd3));
                    g.drawText (juce::String (k / 4 + 1), (int) x + 3, 0, 40, 14, juce::Justification::left);
                }
            }

            // limit of the count-in (earlier audio is not played) and start of the music
            g.setColour (juce::Colour (0xffe5484d).withAlpha (0.8f));
            vline (g, cx0, 0.0f, (float) waveH);
            g.setColour (juce::Colour (0xff2fb5a2));
            vline (g, zeroX, 0.0f, (float) waveH);
            vline (g, zeroX + 1, 0.0f, (float) waveH);

            // playhead
            if ((engine.isPlaying() || engine.isPaused()) && len > 0.0)
            {
                const float x = timeToX (engine.getClockSeconds());
                g.setColour (juce::Colour (0xffffd24d));
                vline (g, x, 0.0f, (float) waveH);
            }

            if (len <= 0.0)
            {
                g.setColour (juce::Colours::grey);
                g.setFont (juce::FontOptions (16.0f));
                g.drawText ("Нажмите \"Добавить аудио\""_ru, 0, 0, getWidth(), waveH, juce::Justification::centred);
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
                    const double step = beatSeconds() / 4.0;
                    value = std::round (value / step) * step;
                }
                track->setClipOffset (value);   // limited to 4 beats before the first bar
            }
            updateScrollbar();
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
            updateScrollbar();
            repaint();
        }

    private:
        static constexpr int scrollH = 16;

        double beatSeconds() const { return 60.0 / juce::jlimit (20.0, 400.0, engine.getBpm()); }

        void timerCallback() override
        {
            updateScrollbar();
            repaint();
        }

        void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override
        {
            viewStart = newRangeStart;
            repaint();
        }

        void updateScrollbar()
        {
            const double len = thumbnail.getTotalLength();
            const double offset = track->getClipOffset();
            const double lo = juce::jmin (viewStart, juce::jmin (-4.0 * beatSeconds(), offset) - 1.0);
            const double hi = juce::jmax (viewStart + viewSpan, offset + len + 2.0);
            scrollbar.setRangeLimits (lo, hi);
            scrollbar.setCurrentRange (viewStart, viewSpan, juce::dontSendNotification);
        }

        float timeToX (double t) const { return (float) ((t - viewStart) / viewSpan * getWidth()); }
        double xToTime (float x) const { return viewStart + (double) x / (double) juce::jmax (1, getWidth()) * viewSpan; }

        Engine& engine;
        std::shared_ptr<Track> track;
        juce::AudioThumbnailCache cache { 4 };
        juce::AudioThumbnail thumbnail;
        juce::ScrollBar scrollbar { false };

        double viewStart = -2.5, viewSpan = 12.0;
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

            playButton.setTooltip ("Играть (если стоит пауза - продолжить; иначе этот плеер подключится ко времени общего плеера)"_ru);
            playButton.onClick = [this]
            {
                if (engine.isPaused())
                    engine.playAll();
                else
                    engine.playTrack (track);
            };
            pauseButton.setTooltip ("Пауза: все плееры и метроном замирают, Play продолжит с этого места"_ru);
            pauseButton.onClick = [this] { engine.pauseAll(); };
            stopButton.setTooltip ("Стоп этого плеера (метроном продолжает идти)"_ru);
            stopButton.onClick = [this] { track->stopPlayer(); };
            addAndMakeVisible (playButton);
            addAndMakeVisible (pauseButton);
            addAndMakeVisible (stopButton);

            seekSlider.setSliderStyle (juce::Slider::LinearHorizontal);
            seekSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            seekSlider.setRange (0.0, 1.0, 0.0);
            seekSlider.setTooltip ("Перемотка песни: схватите ползунок и тяните"_ru);
            seekSlider.onDragStart = [this] { seeking = true; };
            seekSlider.onDragEnd = [this]
            {
                seeking = false;
                engine.seek (seekSlider.getValue());
            };
            seekSlider.onValueChange = [this]
            {
                // while dragging, follow the thumb (limited to ~20 times per second)
                const auto now = juce::Time::getMillisecondCounter();
                if (seeking && now - lastSeekMs > 50)
                {
                    lastSeekMs = now;
                    engine.seek (seekSlider.getValue());
                }
            };
            addAndMakeVisible (seekSlider);
            timeLabel.setJustificationType (juce::Justification::centredRight);
            timeLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            addAndMakeVisible (timeLabel);

            loopButton.setToggleState (track->isLooping(), juce::dontSendNotification);
            loopButton.onClick = [this] { track->setLooping (loopButton.getToggleState()); };
            addAndMakeVisible (loopButton);

            snapButton.setToggleState (false, juce::dontSendNotification);
            snapButton.onClick = [this] { wave.snapToGrid = snapButton.getToggleState(); };
            addAndMakeVisible (snapButton);

            tempoButton.setButtonText ("Определить темп"_ru);
            tempoButton.setTooltip ("Определить темп по этому файлу и установить его как темп проекта"_ru);
            tempoButton.onClick = [this] { detectTempoAsync(); };
            addAndMakeVisible (tempoButton);

            resetButton.setButtonText ("Сбросить сдвиг"_ru);
            resetButton.onClick = [this] { track->setClipOffset (0.0); };
            addAndMakeVisible (resetButton);

            fileLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            addAndMakeVisible (fileLabel);
            offsetLabel.setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (offsetLabel);

            hint.setText ("Колесо мыши - масштаб, ползунок снизу - прокрутка. Перетаскивание - сдвиг аудио по сетке метронома "_ru
                          "(Shift - по долям). Зелёная линия - начало музыки (5-й удар), красная - предел: всё, что левее "_ru
                          "4 ударов отсчёта, не воспроизводится. Без метронома песня играет с самого начала."_ru,
                          juce::dontSendNotification);
            hint.setColour (juce::Label::textColourId, juce::Colours::grey);
            hint.setFont (juce::FontOptions (12.0f));
            hint.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (hint);

            addAndMakeVisible (wave);
            startTimerHz (20);
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
            pauseButton.setBounds (top.removeFromLeft (40));
            top.removeFromLeft (4);
            stopButton.setBounds (top.removeFromLeft (40));
            top.removeFromLeft (10);
            loopButton.setBounds (top.removeFromLeft (90));
            snapButton.setBounds (top.removeFromLeft (190));
            tempoButton.setBounds (top.removeFromLeft (150));
            resetButton.setBounds (top.removeFromRight (150));
            top.removeFromRight (6);
            offsetLabel.setBounds (top.removeFromRight (150));

            r.removeFromTop (4);
            fileLabel.setBounds (r.removeFromTop (20));
            r.removeFromTop (4);
            auto seekRow = r.removeFromTop (24);
            timeLabel.setBounds (seekRow.removeFromRight (120));
            seekSlider.setBounds (seekRow);
            r.removeFromTop (4);
            hint.setBounds (r.removeFromBottom (48));
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

        void detectTempoAsync()
        {
            const auto f = track->getFile();
            if (f == juce::File())
            {
                tempoStatus = "Сначала добавьте аудио"_ru;
                return;
            }

            tempoStatus = "Определяю темп..."_ru;
            tempoButton.setEnabled (false);

            juce::Component::SafePointer<PlayerPanel> safe (this);
            Engine* eng = &engine;
            juce::Thread::launch ([safe, eng, f]
            {
                double bpm = 0.0;
                {
                    juce::AudioFormatManager fm;
                    fm.registerBasicFormats();
                    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
                    if (reader != nullptr)
                        bpm = detectTempo (*reader);
                }

                juce::MessageManager::callAsync ([safe, eng, bpm]
                {
                    if (safe == nullptr)
                        return;
                    safe->tempoButton.setEnabled (true);
                    if (bpm > 0.0)
                    {
                        eng->setBpm (bpm);
                        safe->tempoStatus = "Темп проекта: "_ru + juce::String (bpm, 1);
                    }
                    else
                        safe->tempoStatus = "Темп определить не удалось"_ru;
                });
            });
        }

        void timerCallback() override
        {
            const auto f = track->getFile();
            fileLabel.setText ((f != juce::File() ? f.getFileName() + "   ("_ru + juce::String (track->getLengthSeconds(), 1) + " с)"_ru
                                                  : "Файл не выбран"_ru)
                                   + (tempoStatus.isNotEmpty() ? "      " + tempoStatus : juce::String()),
                               juce::dontSendNotification);

            const int ms = (int) std::lround (track->getClipOffset() * 1000.0);
            offsetLabel.setText ("Сдвиг: "_ru + (ms > 0 ? "+" : "") + juce::String (ms) + " мс"_ru, juce::dontSendNotification);
            playButton.setActive (track->isPlaying());
            pauseButton.setActive (engine.isPaused());

            const double total = juce::jmax (1.0, track->getClipOffset() + track->getLengthSeconds());
            seekSlider.setRange (0.0, total, 0.0);
            const double pos = juce::jlimit (0.0, total, engine.getClockSeconds());
            if (! seeking)
                seekSlider.setValue (pos, juce::dontSendNotification);

            auto mmss = [] (double sec)
            {
                const int t = (int) std::floor (juce::jmax (0.0, sec));
                return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
            };
            timeLabel.setText (mmss (seeking ? seekSlider.getValue() : pos) + " / " + mmss (total), juce::dontSendNotification);
        }

        Engine& engine;
        std::shared_ptr<Track> track;
        juce::TextButton addButton, resetButton, tempoButton;
        juce::String tempoStatus;
        IconButton playButton { IconButton::Icon::Play }, pauseButton { IconButton::Icon::Pause },
                   stopButton { IconButton::Icon::Stop };
        juce::Slider seekSlider;
        juce::Label timeLabel;
        bool seeking = false;
        juce::uint32 lastSeekMs = 0;
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
