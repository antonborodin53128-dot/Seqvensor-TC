#include "Engine.h"
#include "Stretch.h"
#include "TempoDetect.h"
#include "LiveStretch.h"
#include "KeyDetect.h"

Engine::Engine()
{
    pluginFormats.addFormat (std::make_unique<juce::VST3PluginFormat>());
    order.reserve (256);
}

Engine::~Engine()
{
    shutdown();
}

juce::File Engine::getDataDirectory() const
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("TC");
    dir.createDirectory();
    return dir;
}

void Engine::initialise()
{
    // saved plugin list
    auto listFile = getDataDirectory().getChildFile ("plugins.xml");
    if (auto xml = juce::XmlDocument::parse (listFile))
        knownPlugins.recreateFromXml (*xml);

    loadEnabledPlugins();
    loadAnalysisCache();

    // saved audio setup
    std::unique_ptr<juce::XmlElement> audioState = juce::XmlDocument::parse (getDataDirectory().getChildFile ("audio.xml"));
    auto error = deviceManager.initialise (64, 64, audioState.get(), true);
    if (error.isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr)
    {
        // saved / requested setup did not open: fall back to the system default, then to output only
        juce::Logger::writeToLog ("Audio init: " + error);
        error = deviceManager.initialise (2, 2, nullptr, true);
        if (deviceManager.getCurrentAudioDevice() == nullptr)
            error = deviceManager.initialise (0, 2, nullptr, true);
    }
    audioInitError = error;

    {   // "keyMode tempoMode left right": how key and tempo are found, and the input channel for the live mode
        const auto parts = juce::StringArray::fromTokens (getDataDirectory().getChildFile ("analysis.txt").loadFileAsString().trim(), " ", "");
        if (parts.size() >= 4)
        {
            keyMode = juce::jlimit (0, 2, parts[0].getIntValue());
            tempoMode = juce::jlimit (0, 2, parts[1].getIntValue());
            if (parts.size() >= 5)
                liveTrackId = parts[4].getIntValue();
            applyAnalysisModes();
        }
    }

    deviceManager.addAudioCallback (this);
}

void Engine::saveLiveKeySettings() const
{
    getDataDirectory().getChildFile ("analysis.txt").replaceWithText (juce::String (keyMode.load()) + " " + juce::String (tempoMode.load())
                                                                      + " -1 -1 " + juce::String (liveTrackId.load()));
}

void Engine::syncLiveSource()
{
    int l = -1, r = -1;
    for (auto& t : getTracks())
    {
        if (t->inputL.load() < 0 || t->busId.load() >= 0)
            continue;
        if (liveTrackId.load() < 0 || t->id == liveTrackId.load())
        {
            l = t->inputL.load();
            r = t->inputR.load();
            break;
        }
    }
    int cl = -1, cr = -1;
    liveKey.getChannels (cl, cr);
    if (cl != l || cr != r)
    {
        liveKey.setChannels (l, r);
        liveKey.reset();
    }
}

void Engine::shutdown()
{
    if (! running && tracks.empty())
        return;

    saveActiveSequence();

    if (auto xml = deviceManager.createStateXml())
        xml->writeTo (getDataDirectory().getChildFile ("audio.xml"));

    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();

    {
        const juce::ScopedLock sl (tracksLock);
        tracks.clear();
        sequenceCache.clear();
        activeSequence.reset();
    }
}

void Engine::savePluginList() const
{
    if (auto xml = knownPlugins.createXml())
        xml->writeTo (getDataDirectory().getChildFile ("plugins.xml"));
}

// ---------------------------------------------------------------- tracks

std::shared_ptr<Track> Engine::makeTrack (const juce::String& name, int id)
{
    auto track = std::make_shared<Track> (name);
    track->id = id;
    track->setTempoSource (&playHead.bpm);
    track->setPitchSource (&transposeSemis);
    track->prepare (currentSampleRate, currentBlockSize, &playHead);
    return track;
}

std::shared_ptr<Track> Engine::addTrack (const juce::String& name, int forcedId)
{
    auto track = makeTrack (name, forcedId > 0 ? forcedId : nextTrackId);
    nextTrackId = juce::jmax (nextTrackId, track->id + 1);
    {
        const juce::ScopedLock sl (tracksLock);
        tracks.push_back (track);
    }
    if (onTracksChanged)
        onTracksChanged();
    return track;
}

std::shared_ptr<Track> Engine::findTrack (int id) const
{
    const juce::ScopedLock sl (tracksLock);
    for (auto& t : tracks)
        if (t->id == id)
            return t;
    return {};
}

bool Engine::wouldCreateCycle (const Track& track, int sourceId) const
{
    int cursor = sourceId;
    for (int guard = 0; guard < 64 && cursor >= 0; ++guard)
    {
        if (cursor == track.id)
            return true;
        auto t = findTrack (cursor);
        if (t == nullptr)
            return false;
        cursor = t->busId.load();
    }
    return false;
}

void Engine::removeTrack (const std::shared_ptr<Track>& track)
{
    {
        const juce::ScopedLock sl (tracksLock);
        tracks.erase (std::remove (tracks.begin(), tracks.end(), track), tracks.end());
        for (auto& t : tracks)
            if (t->busId.load() == track->id)
                t->busId = -1;
    }
    clipsChanged();
    if (onTracksChanged)
        onTracksChanged();
}

std::vector<std::shared_ptr<Track>> Engine::getTracks() const
{
    const juce::ScopedLock sl (tracksLock);
    return tracks;
}

void Engine::addPluginToTrack (const std::shared_ptr<Track>& track, int slotIndex, const juce::PluginDescription& description,
                               std::function<void (bool, const juce::String&)> done,
                               const juce::String& savedStateBase64, bool bypassed)
{
    std::weak_ptr<Track> weak = track;
    pluginFormats.createPluginInstanceAsync (
        description, currentSampleRate, currentBlockSize,
        [this, weak, slotIndex, description, done, savedStateBase64, bypassed]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            auto trackPtr = weak.lock();
            if (instance == nullptr || trackPtr == nullptr)
            {
                if (done)
                    done (false, error.isNotEmpty() ? error : juce::String ("Track was removed"));
                return;
            }

            // Prefer a plain stereo layout on the main buses.
            auto layout = instance->getBusesLayout();
            if (instance->getBusCount (true) > 0 && instance->getBus (true, 0)->isNumberOfChannelsSupported (2))
                layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
            if (instance->getBusCount (false) > 0 && instance->getBus (false, 0)->isNumberOfChannelsSupported (2))
                layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
            instance->setBusesLayout (layout);

            if (savedStateBase64.isNotEmpty())
            {
                juce::MemoryOutputStream decoded;
                if (juce::Base64::convertFromBase64 (decoded, savedStateBase64))
                    instance->setStateInformation (decoded.getData(), (int) decoded.getDataSize());
            }

            instance->setPlayHead (&playHead);
            instance->setRateAndBufferSizeDetails (currentSampleRate, currentBlockSize);
            instance->prepareToPlay (currentSampleRate, currentBlockSize);

            trackPtr->setPlugin (slotIndex, std::move (instance), description);
            if (auto* slot = trackPtr->getSlot (slotIndex))
                slot->bypass.store (bypassed);
            trackPtr->refreshLatency();

            if (done)
                done (true, {});
        });
}

// ---------------------------------------------------------------- transport

void Engine::logLine (const juce::String& m) const
{
    getDataDirectory().getChildFile ("log.txt").appendText (juce::Time::getCurrentTime().toString (false, true) + " " + m + "\n");
}

void Engine::playAll()
{
    logLine ("playAll: " + describeState());
    if (clockRunning.load() || command.load() == 1)
        return;   // already playing

    double start = 0.0;
    if (paused.load())
    {
        start = clockNow.load();   // resume where we paused
    }
    else
    {
        resetShifts.store (true);
        bool anyMetronome = false;
        double earliest = 0.0;
        for (auto& t : getTracks())
        {
            anyMetronome = anyMetronome || t->metronomeOn.load();
            earliest = juce::jmin (earliest, t->getEarliestStart());
        }

        // metronome on: 4 count-in beats. Otherwise start where the first clip starts (at most 4 beats before bar 1).
        const double bar = 4.0 * getBeatSeconds();
        start = anyMetronome ? -bar : juce::jmax (-bar, earliest);
    }

    paused.store (false);
    commandClock.store (start);
    command.store (1);
}

void Engine::setTempo (double newBpm)
{
    newBpm = juce::jlimit (20.0, 400.0, newBpm);
    if (isAudioAlive())
    {
        requestedBpm.store (newBpm);   // the audio thread picks it up at the start of the next block
        return;
    }
    const juce::ScopedLock sl (tracksLock);
    applyTempoChange (newBpm);
}

void Engine::applyTempoChange (double newBpm)
{
    const double oldBpm = playHead.bpm.load();
    if (std::abs (newBpm - oldBpm) < 1.0e-9)
        return;

    const double r = oldBpm / newBpm;
    const bool live = clockRunning.load();
    const double c = live ? clockSeconds : clockNow.load();

    for (auto& t : tracks)
        t->retempo (r, c, newBpm);

    if (live)
    {
        clockSeconds *= r;                    // the position in bars stays where it is
        nextBeatIn *= r;                      // the running beat keeps its phase: no double or missing click
        clockNow.store (clockSeconds);
    }
    else
    {
        clockNow.store (c * r);
    }
    if (command.load() == 1)
        commandClock.store (commandClock.load() * r);

    playHead.bpm.store (newBpm);
    tempoApplied.store (true);
}

void Engine::pauseAll()
{
    logLine ("pauseAll: " + describeState());
    if (! (clockRunning.load() || command.load() == 1))
        return;

    command.store (0);
    clockRunning.store (false);
    paused.store (true);
}

void Engine::stopAll()
{
    logLine ("stopAll: " + describeState());
    command.store (0);
    clockRunning.store (false);
    paused.store (false);
    clockNow.store (0.0);
    resetShifts.store (true);
}

void Engine::seek (double clockSecondsTarget)
{
    // from 4 beats before bar 1 (the count-in) to a little after the end of the song
    const double t = juce::jlimit (-4.0 * getBeatSeconds(), juce::jmax (60.0, songEnd.load() + 30.0), clockSecondsTarget);
    resetShifts.store (true);

    if (clockRunning.load() || command.load() == 1)
    {
        commandClock.store (t);
        command.store (1);
    }
    else
    {
        paused.store (true);   // the next Play continues from here
        clockNow.store (t);
    }
}

juce::String Engine::describeState() const
{
    juce::String d;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        d << dev->getTypeName() << " / " << dev->getName() << " " << dev->getCurrentSampleRate() << "Hz blk "
          << dev->getCurrentBufferSizeSamples() << " playing=" << (int) dev->isPlaying();
    else
        d << "NO DEVICE";
    d << " | cb=" << (juce::int64) callbackCount.load() << " lockFail=" << (juce::int64) lockFailures.load()
      << " clock=" << clockNow.load() << " running=" << (int) clockRunning.load() << " cmd=" << command.load()
      << " paused=" << (int) paused.load() << " songEnd=" << songEnd.load() << " bpm=" << getBpm()
      << " stopReason=" << stopReason.load() << " stopClock=" << stopClock.load() << " tracks=" << (int) tracks.size() << " engineRunning=" << (int) running;
    return d;
}

bool Engine::isAudioAlive() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr && device->isPlaying() && running;
}

bool Engine::isPlaying() const
{
    return clockRunning.load() || command.load() == 1;
}

// ---------------------------------------------------------------- audio

juce::String Engine::describeDeviceLatency() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const double rate = device->getCurrentSampleRate();
        if (rate > 0.0)
        {
            const double in = device->getInputLatencyInSamples() * 1000.0 / rate;
            const double out = device->getOutputLatencyInSamples() * 1000.0 / rate;
            const double buf = device->getCurrentBufferSizeSamples() * 1000.0 / rate;
            return "буфер "_ru + juce::String (buf, 1) + " мс, вход "_ru + juce::String (in, 1)
                   + " мс, выход "_ru + juce::String (out, 1) + " мс"_ru;
        }
    }
    return "устройство не запущено"_ru;
}

void Engine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    currentSampleRate = device->getCurrentSampleRate();
    currentBlockSize = device->getCurrentBufferSizeSamples();
    playHead.sampleRate.store (currentSampleRate);
    loadMeasure.reset (currentSampleRate, currentBlockSize);
    liveKey.prepare (currentSampleRate);

    const juce::ScopedLock sl (tracksLock);
    for (auto& t : tracks)
        t->prepare (currentSampleRate, currentBlockSize, &playHead);
    for (auto& kv : sequenceCache)
        for (auto& t : kv.second->tracks)
            t->prepare (currentSampleRate, currentBlockSize, &playHead);
    running = true;

    // live stretchers are built for one device rate: rebuild the ones that no longer match
    juce::MessageManager::callAsync ([this]
    {
        std::vector<std::shared_ptr<Track>> all = getTracks();
        {
            const juce::ScopedLock sl (tracksLock);
            for (auto& kv : sequenceCache)
                for (auto& t : kv.second->tracks)
                    all.push_back (t);
        }
        for (auto& t : all)
            for (auto& c : t->getClips())
                ensureLive (t, c);
    });
}

void Engine::audioDeviceStopped()
{
    const juce::ScopedLock sl (tracksLock);
    for (auto& t : tracks)
        t->release();
    for (auto& kv : sequenceCache)
        for (auto& t : kv.second->tracks)
            t->release();
    running = false;
}

void Engine::updateCompensation()
{
    int maxLatency = 0;
    for (auto& t : tracks)
        maxLatency = juce::jmax (maxLatency, t->getCachedLatencySamples());

    for (auto& t : tracks)
        t->setExtraDelay (t->compensate.load() ? maxLatency - t->getCachedLatencySamples() : 0);
}

void Engine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                               float* const* outputChannelData, int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;
    callbackCount.fetch_add (1);
    const juce::AudioProcessLoadMeasurer::ScopedTimer timer (loadMeasure, numSamples);

    for (int c = 0; c < numOutputChannels; ++c)
        if (outputChannelData[c] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[c], numSamples);

    liveKey.push (inputChannelData, numInputChannels, numSamples);

    const juce::ScopedTryLock lock (tracksLock);
    if (! lock.isLocked())
    {
        lockFailures.fetch_add (1);
        return;
    }

    updateCompensation();

    if (resetShifts.exchange (false))
        for (auto& t : tracks)
            t->clearShifts();

    if (const double req = requestedBpm.exchange (0.0); req > 0.0)
        applyTempoChange (req);

    // (re)start / jump of the global clock requested by the UI
    if (command.exchange (0) == 1)
    {
        clockSeconds = commandClock.load();
        const double beatSamples = currentSampleRate * 60.0 / juce::jlimit (20.0, 400.0, getBpm());
        const double beatPos = clockSeconds * getBpm() / 60.0;
        const double nextBeat = std::ceil (beatPos - 1.0e-6);
        beatIndex = (int) nextBeat;
        nextBeatIn = (nextBeat - beatPos) * beatSamples;
        clockRunning.store (true);
    }

    // tracks that read another track (bus) go last so they get the fresh block
    order.clear();
    for (auto& t : tracks)
        if (t->busId.load() < 0)
            order.push_back (t.get());
    for (auto& t : tracks)
        if (t->busId.load() >= 0)
            order.push_back (t.get());

    const int chunk = juce::jmax (64, currentBlockSize);
    int done = 0;
    while (done < numSamples)
    {
        const int n = juce::jmin (chunk, numSamples - done);

        ChunkClock clock;
        clock.running = clockRunning.load();
        clock.startSeconds = clockSeconds;

        if (clock.running)
        {
            const double beatSamples = currentSampleRate * 60.0 / juce::jlimit (20.0, 400.0, getBpm());
            int pos = 0;
            while (pos < n)
            {
                if (nextBeatIn <= 0.0)
                {
                    if (clock.numEvents < ChunkClock::maxEvents)
                    {
                        clock.eventOffset[clock.numEvents] = pos;
                        clock.eventAccent[clock.numEvents] = (((beatIndex % 4) + 4) % 4) == 0;
                        ++clock.numEvents;
                    }
                    ++beatIndex;
                    nextBeatIn += beatSamples;
                }
                const int seg = juce::jmin (n - pos, juce::jmax (1, (int) std::ceil (nextBeatIn)));
                nextBeatIn -= (double) seg;
                pos += seg;
            }
        }

        for (auto* t : order)
        {
            const Track* source = nullptr;
            const int b = t->busId.load();
            if (b >= 0)
                for (auto* o : order)
                    if (o->id == b)
                        source = o;
            t->processChunk (inputChannelData, numInputChannels, outputChannelData, numOutputChannels,
                             done, n, source, clock);
        }

        if (clock.running)
        {
            clockSeconds += (double) n / currentSampleRate;
            clockNow.store (clockSeconds);
        }
        done += n;
    }

    // the last clip has ended: stop the clock (and with it the metronome) and rewind
    const double end = songEnd.load();
    if (clockRunning.load() && end > 0.0 && clockSeconds > end + 0.3)
    {
        stopReason.store (1);
        stopClock.store (clockSeconds);
        clockRunning.store (false);
        clockNow.store (0.0);
    }

    playHead.playing.store (clockRunning.load());
    playHead.advance (numSamples);
}

// ---------------------------------------------------------------- plugin selection

void Engine::loadEnabledPlugins()
{
    auto f = getDataDirectory().getChildFile ("enabled-plugins.txt");
    if (! f.existsAsFile())
    {
        allPluginsEnabled = true;
        return;
    }
    allPluginsEnabled = false;
    enabledPlugins.clear();
    juce::StringArray lines;
    lines.addLines (f.loadFileAsString());
    for (auto& l : lines)
        if (l.trim().isNotEmpty())
            enabledPlugins.insert (l.trim());
}

void Engine::saveEnabledPlugins() const
{
    juce::StringArray lines;
    for (auto& s : enabledPlugins)
        lines.add (s);
    getDataDirectory().getChildFile ("enabled-plugins.txt").replaceWithText (lines.joinIntoString ("\n"), false, false, "\n");
}

bool Engine::isPluginEnabled (const juce::PluginDescription& d) const
{
    return allPluginsEnabled || enabledPlugins.count (d.createIdentifierString()) > 0;
}

void Engine::setPluginEnabled (const juce::PluginDescription& d, bool enabled)
{
    if (allPluginsEnabled)
    {
        allPluginsEnabled = false;
        enabledPlugins.clear();
        for (auto& t : knownPlugins.getTypes())
            enabledPlugins.insert (t.createIdentifierString());
    }

    if (enabled)
        enabledPlugins.insert (d.createIdentifierString());
    else
        enabledPlugins.erase (d.createIdentifierString());
    saveEnabledPlugins();
}

void Engine::setAllPluginsEnabled (bool enabled)
{
    allPluginsEnabled = false;
    enabledPlugins.clear();
    if (enabled)
        for (auto& t : knownPlugins.getTypes())
            enabledPlugins.insert (t.createIdentifierString());
    saveEnabledPlugins();
}

std::vector<juce::PluginDescription> Engine::getEnabledEffects() const
{
    std::vector<juce::PluginDescription> result;
    for (auto& d : knownPlugins.getTypes())
        if (! d.isInstrument && isPluginEnabled (d))
            result.push_back (d);

    std::sort (result.begin(), result.end(), [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
    {
        return a.name.compareIgnoreCase (b.name) < 0;
    });
    return result;
}

// ---------------------------------------------------------------- clips

void Engine::clipsChanged()
{
    double end = 0.0;
    for (auto& t : getTracks())
        end = juce::jmax (end, t->getEndSeconds());
    songEnd.store (end);
}

juce::String Engine::cacheKey (const juce::File& f)
{
    return "v1|" + f.getFullPathName() + "|" + juce::String (f.getSize()) + "|" + juce::String (f.getLastModificationTime().toMilliseconds());
}

bool Engine::findCached (const juce::String& key, FileAnalysis& out)
{
    const juce::ScopedLock sl (cacheLock);
    auto it = analysisCache.find (key);
    if (it == analysisCache.end())
        return false;
    out = it->second;
    return true;
}

void Engine::storeCached (const juce::String& key, const FileAnalysis& a)
{
    juce::String line = key + "\t" + juce::String (a.bpm, 4) + "\t" + juce::String (a.confidence, 4) + "\t" + juce::String (a.mass, 4);
    for (float h : a.hist)
        line += "\t" + juce::String (h, 4);

    const juce::ScopedLock sl (cacheLock);
    analysisCache[key] = a;
    if (auto out = getDataDirectory().getChildFile ("analysis-cache.txt").createOutputStream())
    {
        out->writeText (line + "\n", false, false, nullptr);
    }
}

void Engine::loadAnalysisCache()
{
    auto file = getDataDirectory().getChildFile ("analysis-cache.txt");
    // append mode: reopen at the end when writing; reading is a plain pass
    juce::StringArray lines;
    file.readLines (lines);
    const juce::ScopedLock sl (cacheLock);
    for (auto& l : lines)
    {
        const auto f = juce::StringArray::fromTokens (l, "\t", "");
        if (f.size() < 16)
            continue;
        FileAnalysis a;
        a.bpm = f[1].getDoubleValue();
        a.confidence = f[2].getDoubleValue();
        a.mass = (float) f[3].getDoubleValue();
        for (int i = 0; i < 12; ++i)
            a.hist[i] = (float) f[4 + i].getDoubleValue();
        analysisCache[f[0]] = a;
    }
}

void Engine::applyAnalysis (ClipData& d, const FileAnalysis& a)
{
    d.tempoConfidence = a.confidence;
    d.fileBpm = a.confidence >= tempoMinConfidence ? a.bpm : 0.0;
    d.keyMass = a.mass;
    std::copy (a.hist, a.hist + 12, d.keyHist);
}

void Engine::addClipFromFile (const std::shared_ptr<Track>& track, const juce::File& file, double startSeconds,
                              std::function<void (std::shared_ptr<Clip>, const juce::String&)> done,
                              std::function<void (std::shared_ptr<Clip>)> analysed)
{
    track->loadingClips++;
    std::weak_ptr<Track> weak = track;

    juce::Thread::launch ([this, weak, file, startSeconds, done, analysed]
    {
        juce::String error;
        auto data = ClipData::load (file, error);
        std::shared_ptr<LiveStretch> live;
        const auto key = cacheKey (file);
        FileAnalysis cached;
        const bool hit = data != nullptr && findCached (key, cached);
        if (hit)
        {
            applyAnalysis (*data, cached);   // seen before: no waiting
            live = std::make_shared<LiveStretch> (data, currentSampleRate, data->fileBpm > 0.0 || transposeSemis.load() != 0);
        }

        juce::MessageManager::callAsync ([this, weak, data, live, error, startSeconds, done, analysed, hit, key, file]
        {
            auto t = weak.lock();
            if (t == nullptr)
                return;
            t->loadingClips--;

            std::shared_ptr<Clip> clip;
            if (data != nullptr)
            {
                // a playback track has no live input: the default input would only add noise
                if (! t->inputChosenByUser)
                    t->inputL = -1;
                clip = t->addClip (data, startSeconds, takeClipId(), live);
                clip->updateSpeed (getBpm());
                clipsChanged();
            }
            if (done)
                done (clip, error);
            if (clip == nullptr)
                return;
            if (hit)
            {
                if (analysed)
                    analysed (clip);
                if (onAnalysisDone)
                    onAnalysisDone();
                return;
            }

            // tempo and key: slow, so the clip is already on the track while they are computed
            std::weak_ptr<Clip> weakClip = clip;
            analysisPool.addJob ([this, weak, weakClip, data, key, analysed]
            {
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                std::vector<float> mono;
                data->getMono (mono, (int) (data->sampleRate * 150.0));
                const auto found = detectTempoEx (mono, data->sampleRate);
                const auto t1 = juce::Time::getMillisecondCounterHiRes();

                // the key is judged from up to 90 s from the middle of the file
                const size_t keep = (size_t) (data->sampleRate * 90.0);
                KeyHist kh;
                if (mono.size() > keep)
                {
                    const auto from = mono.begin() + (long) ((mono.size() - keep) / 2);
                    kh = computeKeyHist (std::vector<float> (from, from + (long) keep), data->sampleRate);
                }
                else
                    kh = computeKeyHist (mono, data->sampleRate);
                const auto t2 = juce::Time::getMillisecondCounterHiRes();

                FileAnalysis a;
                a.bpm = found.bpm;
                a.confidence = found.confidence;
                if (kh.valid)
                {
                    a.mass = kh.mass;
                    std::copy (kh.v, kh.v + 12, a.hist);
                }
                if (kh.valid || keyEngineError().isEmpty())
                    storeCached (key, a);   // a failed network must not be remembered as "no notes"
                logLine ("analysis " + data->name + ": tempo " + juce::String ((int) (t1 - t0)) + " ms, key " + juce::String ((int) (t2 - t1))
                         + " ms (" + keyEngineError() + ")");

                juce::MessageManager::callAsync ([this, weak, weakClip, data, a, analysed]
                {
                    auto c = weakClip.lock();
                    if (c == nullptr)
                        return;
                    applyAnalysis (*data, a);
                    c->updateSpeed (getBpm());
                    if (auto owner = weak.lock())
                        ensureLive (owner, c);
                    if (analysed)
                        analysed (c);
                    if (onAnalysisDone)
                        onAnalysisDone();
                });
            });
        });
    });
}

// Makes sure a clip has a live stretcher for the current device rate (built in the background).
void Engine::ensureLive (const std::shared_ptr<Track>& track, const std::shared_ptr<Clip>& clip)
{
    // needed for tempo following (tempo known) or for the transposition of the mix
    if (clip->original == nullptr || (clip->original->fileBpm <= 0.0 && transposeSemis.load() == 0)
        || (clip->live != nullptr && clip->live->isReady (currentSampleRate)))
        return;
    if (clip->liveBuilding.exchange (true))
        return;

    std::weak_ptr<Track> weakTrack = track;
    std::weak_ptr<Clip> weakClip = clip;
    juce::Thread::launch ([this, weakTrack, weakClip, original = clip->original, rate = currentSampleRate]
    {
        auto fresh = std::make_shared<LiveStretch> (original, rate, true);
        juce::MessageManager::callAsync ([weakTrack, weakClip, fresh]
        {
            auto c = weakClip.lock();
            if (c == nullptr)
                return;
            if (auto t = weakTrack.lock())
                t->setLive (c, fresh);
            c->liveBuilding.store (false);
        });
    });
}

// Key of the whole mix: the note histograms of all tonal clips add up (drums and noise are left out).
KeyGuess Engine::getProjectKey (int* clipsUsed)
{
    float sum[12] = {};
    int used = 0;
    for (auto& t : getTracks())
    {
        if (t->muted.load())
            continue;   // a switched-off track does not take part in the key
        for (auto& c : t->getClips())
        {
            const ClipData* d = c->original.get();
            if (d == nullptr || d->keyMass <= 1.0f)
                continue;
            float h[12];
            for (int i = 0; i < 12; ++i)
                h[i] = d->keyHist[i] / d->keyMass;
            const KeyGuess g = guessKey (h);
            if (g.corr < 0.5)
                continue;   // not tonal enough: percussion, noise
            for (int i = 0; i < 12; ++i)
                sum[i] += h[i] * (float) g.corr;
            ++used;
        }
    }
    if (clipsUsed != nullptr)
        *clipsUsed = used;
    return used > 0 ? guessKey (sum) : KeyGuess {};
}

// The tempo most clips agree on (files within 1.5 % count as the same tempo), weighted by the detector's confidence.
double Engine::getMixTempo()
{
    struct Vote { double bpm, weight; };
    std::vector<Vote> votes;
    for (auto& t : getTracks())
        for (auto& c : t->getClips())
            if (c->original != nullptr && c->original->fileBpm > 0.0)
                votes.push_back ({ c->original->fileBpm, juce::jmax (0.05, c->original->tempoConfidence) });

    double best = 0.0, bestWeight = 0.0;
    for (auto& a : votes)
    {
        double w = 0.0, sum = 0.0;
        for (auto& b : votes)
            if (std::abs (b.bpm / a.bpm - 1.0) < 0.015)
            {
                w += b.weight;
                sum += b.bpm * b.weight;
            }
        if (w > bestWeight)
        {
            bestWeight = w;
            best = sum / w;
        }
    }
    return best > 0.0 ? std::round (best * 10.0) / 10.0 : 0.0;
}

void Engine::setTranspose (int semitones)
{
    semitones = juce::jlimit (-12, 12, semitones);
    if (semitones == transposeSemis.load())
        return;
    transposeSemis.store (semitones);
    if (semitones != 0)
        for (auto& t : getTracks())
            for (auto& c : t->getClips())
                ensureLive (t, c);   // the real-time stretcher is also what shifts the pitch
    if (activeSequence != nullptr)
        activeSequence->semitones = semitones;
    clipsChanged();
}

void Engine::refreshClipSpeeds()
{
    const double bpm = getBpm();
    for (auto& t : getTracks())
        for (auto& c : t->getClips())
            c->updateSpeed (bpm);
}

void Engine::stretchClip (const std::shared_ptr<Track>& track, const std::shared_ptr<Clip>& clip, double targetBpm,
                          std::function<void()> done)
{
    const double sourceBpm = clip->original != nullptr ? clip->original->fileBpm : 0.0;
    if (sourceBpm <= 0.0 || targetBpm <= 0.0)
    {
        if (done)
            done();
        return;
    }

    const double ratio = juce::jlimit (0.25, 4.0, sourceBpm / targetBpm);   // < 1: the file is slower than the project
    const bool untouched = std::abs (ratio - 1.0) < 0.0005;
    clip->stretchTarget.store (targetBpm);
    const int gen = ++clip->stretchGen;

    std::weak_ptr<Track> weakTrack = track;
    std::weak_ptr<Clip> weakClip = clip;
    juce::Thread::launch ([this, weakTrack, weakClip, original = clip->original, ratio, untouched, targetBpm, gen, done]
    {
        std::lock_guard<std::mutex> lock (stretchMutex);
        auto isStale = [weakClip, gen] { auto c = weakClip.lock(); return c == nullptr || c->stretchGen.load() != gen; };
        if (isStale())
            return;

        auto result = untouched ? original : stretchClipData (*original, ratio, isStale);

        juce::MessageManager::callAsync ([this, weakTrack, weakClip, result, untouched, targetBpm, gen, done]
        {
            auto t = weakTrack.lock();
            auto c = weakClip.lock();
            if (t == nullptr || c == nullptr || result == nullptr || c->stretchGen.load() != gen)
                return;
            t->replaceClipData (c, result, clockNow.load());
            c->stretchedToBpm.store (untouched ? 0.0 : targetBpm);
            c->stretchTarget.store (0.0);
            clipsChanged();
            if (done)
                done();
        });
    });
}

void Engine::removeClip (const std::shared_ptr<Track>& track, int clipId)
{
    track->removeClip (clipId);
    clipsChanged();
}

// ---------------------------------------------------------------- sequences (songs)

namespace
{
    void writeSequenceFile (const juce::File& file, const std::vector<std::shared_ptr<Track>>& list, double bpm, int semitones)
    {
        juce::XmlElement root ("TC_PROJECT");
        root.setAttribute ("version", 3);
        root.setAttribute ("bpm", bpm);
        root.setAttribute ("semis", semitones);

        for (auto& t : list)
        {
            auto* te = root.createNewChildElement ("TRACK");
            te->setAttribute ("id", t->id);
            te->setAttribute ("name", t->name);
            te->setAttribute ("inL", t->inputL.load());
            te->setAttribute ("inR", t->inputR.load());
            te->setAttribute ("bus", t->busId.load());
            te->setAttribute ("out", t->outputPair.load());
            te->setAttribute ("gain", (double) t->gainLinear.load());
            te->setAttribute ("mute", t->muted.load());
            te->setAttribute ("transpose", t->transposeOn.load());
            te->setAttribute ("rec", t->recArmed.load());
            te->setAttribute ("comp", t->compensate.load());
            te->setAttribute ("metro", t->metronomeOn.load());
            te->setAttribute ("inputSet", t->inputChosenByUser);

            for (auto& c : t->getClips())
            {
                auto* ce = te->createNewChildElement ("CLIP");
                ce->setAttribute ("file", (c->original != nullptr ? c->original : c->data)->file.getFullPathName());
                ce->setAttribute ("start", c->start.load());
                ce->setAttribute ("loop", c->loop.load());
                ce->setAttribute ("follow", c->followTempo.load());
                ce->setAttribute ("stretchBpm", c->stretchedToBpm.load());
            }

            for (int i = 0; i < Track::numSlots; ++i)
            {
                auto* slot = t->getSlot (i);
                if (slot == nullptr || slot->plugin == nullptr)
                    continue;

                auto* pe = te->createNewChildElement ("PLUGIN");
                pe->setAttribute ("slot", i);
                pe->setAttribute ("bypass", slot->bypass.load());

                if (auto desc = slot->description.createXml())
                    pe->addChildElement (desc.release());

                juce::MemoryBlock state;
                slot->plugin->getStateInformation (state);
                pe->setAttribute ("state", juce::Base64::toBase64 (state.getData(), state.getSize()));
            }
        }

        file.getParentDirectory().createDirectory();
        file.replaceWithText (root.toString());
    }
}

void Engine::saveActiveSequence()
{
    // never write a half-loaded sequence: its missing clips / plugins would be lost
    if (activeSequence == nullptr || activeSequence->pending.load() > 0)
        return;
    writeSequenceFile (activeSequence->file, getTracks(), getBpm(), transposeSemis.load());
}

void Engine::sequenceProgress (const std::shared_ptr<Sequence>& seq)
{
    if (seq == activeSequence)
    {
        clipsChanged();
        if (onTracksChanged)
            onTracksChanged();
    }
}

std::shared_ptr<Sequence> Engine::obtainSequence (const juce::File& file)
{
    const auto key = file.getFullPathName();
    {
        const juce::ScopedLock sl (tracksLock);
        auto it = sequenceCache.find (key);
        if (it != sequenceCache.end())
            return it->second;
    }

    auto seq = std::make_shared<Sequence>();
    seq->file = file;
    seq->name = file.getFileNameWithoutExtension();
    fillSequence (seq);

    const juce::ScopedLock sl (tracksLock);
    sequenceCache[key] = seq;
    return seq;
}

void Engine::fillSequence (const std::shared_ptr<Sequence>& seq)
{
    auto xml = juce::XmlDocument::parse (seq->file);
    if (xml == nullptr || ! xml->hasTagName ("TC_PROJECT"))
        return;

    seq->bpm = xml->getDoubleAttribute ("bpm", 120.0);
    seq->semitones = xml->getIntAttribute ("semis", 0);
    const int version = xml->getIntAttribute ("version", 1);
    std::weak_ptr<Sequence> weakSeq = seq;

    auto finished = [this, weakSeq]
    {
        if (auto s = weakSeq.lock())
        {
            s->pending--;
            sequenceProgress (s);
        }
    };

    int maxId = 0;
    for (auto* te : xml->getChildWithTagNameIterator ("TRACK"))
        maxId = juce::jmax (maxId, te->getIntAttribute ("id", 0));

    for (auto* te : xml->getChildWithTagNameIterator ("TRACK"))
    {
        const int id = te->getIntAttribute ("id", -1);
        auto track = makeTrack (te->getStringAttribute ("name", "Дорожка"_ru), id > 0 ? id : ++maxId);
        seq->tracks.push_back (track);

        track->inputL = te->getIntAttribute ("inL", -1);
        track->inputR = te->getIntAttribute ("inR", -1);
        track->busId = te->getIntAttribute ("bus", -1);
        track->outputPair = te->getIntAttribute ("out", 0);
        track->gainLinear = (float) te->getDoubleAttribute ("gain", 1.0);
        track->muted = te->getBoolAttribute ("mute", false);
        track->transposeOn = te->getBoolAttribute ("transpose", true);
        track->recArmed = te->getBoolAttribute ("rec", false);
        track->compensate = te->getBoolAttribute ("comp", false);
        track->inputChosenByUser = te->getBoolAttribute ("inputSet", true);
        if (version >= 3)
            track->metronomeOn = te->getBoolAttribute ("metro", false);

        for (auto* ce : te->getChildWithTagNameIterator ("CLIP"))
        {
            juce::File f (ce->getStringAttribute ("file"));
            if (! f.existsAsFile())
                continue;
            seq->pending++;
            addClipFromFile (track, f, ce->getDoubleAttribute ("start", 0.0),
                             [this, track, finished, loop = ce->getBoolAttribute ("loop", false),
                              follow = ce->getBoolAttribute ("follow", false), stretchBpm = ce->getDoubleAttribute ("stretchBpm", 0.0),
                              seqBpm = seq->bpm]
                             (std::shared_ptr<Clip> clip, const juce::String&)
                             {
                                 if (clip != nullptr)
                                 {
                                     clip->loop = loop;
                                     clip->followTempo = follow;
                                     clip->updateSpeed (seqBpm);
                                 }
                                 finished();
                             },
                             [this, track, follow = ce->getBoolAttribute ("follow", false), stretchBpm = ce->getDoubleAttribute ("stretchBpm", 0.0)]
                             (std::shared_ptr<Clip> clip)
                             {
                                 // the file's tempo is known now: restore the stretched audio of the clip
                                 if (stretchBpm > 0.0 && ! follow)
                                     stretchClip (track, clip, stretchBpm);
                             });
        }

        // projects from the first versions: a built-in player with a file and an offset
        for (auto* pe : te->getChildWithTagNameIterator ("PLAYER"))
        {
            juce::ignoreUnused (pe);
            juce::File f (te->getStringAttribute ("file"));
            if (f.existsAsFile())
            {
                seq->pending++;
                addClipFromFile (track, f, te->getDoubleAttribute ("offset", 0.0),
                                 [finished] (std::shared_ptr<Clip>, const juce::String&) { finished(); });
            }
        }

        int fallbackSlot = 0;
        for (auto* pe : te->getChildWithTagNameIterator ("PLUGIN"))
        {
            const int slot = pe->hasAttribute ("slot") ? pe->getIntAttribute ("slot") : fallbackSlot++;
            juce::PluginDescription desc;
            if (auto* de = pe->getFirstChildElement())
                if (desc.loadFromXml (*de))
                {
                    seq->pending++;
                    addPluginToTrack (track, slot, desc, [finished] (bool, const juce::String&) { finished(); },
                                      pe->getStringAttribute ("state"), pe->getBoolAttribute ("bypass", false));
                }
        }
    }
}

void Engine::activateSequence (const std::shared_ptr<Sequence>& seq)
{
    if (seq == nullptr || seq == activeSequence)
        return;

    stopAll();

    if (activeSequence != nullptr)
    {
        saveActiveSequence();
        activeSequence->tracks = getTracks();
        activeSequence->bpm = getBpm();
    }

    int maxId = 0;
    for (auto& t : seq->tracks)
        maxId = juce::jmax (maxId, t->id);

    {
        const juce::ScopedLock sl (tracksLock);
        tracks = seq->tracks;
        activeSequence = seq;
    }

    nextTrackId = maxId + 1;
    setBpm (seq->bpm);
    transposeSemis.store (seq->semitones);
    for (auto& t : seq->tracks)
        for (auto& c : t->getClips())
            ensureLive (t, c);
    refreshClipSpeeds();
    clipsChanged();
    if (onTracksChanged)
        onTracksChanged();
}

void Engine::dropSequences (const std::vector<juce::File>& keep)
{
    std::vector<std::shared_ptr<Sequence>> dropped;   // destroyed outside the lock
    {
        const juce::ScopedLock sl (tracksLock);
        for (auto it = sequenceCache.begin(); it != sequenceCache.end();)
        {
            const bool wanted = it->second == activeSequence
                                || std::find (keep.begin(), keep.end(), it->second->file) != keep.end();
            if (wanted)
                ++it;
            else
            {
                dropped.push_back (it->second);
                it = sequenceCache.erase (it);
            }
        }
    }
    dropped.clear();
}

void Engine::forgetSequence (const juce::File& file)
{
    std::shared_ptr<Sequence> gone;
    const juce::ScopedLock sl (tracksLock);
    auto it = sequenceCache.find (file.getFullPathName());
    if (it != sequenceCache.end() && it->second != activeSequence)
    {
        gone = it->second;
        sequenceCache.erase (it);
    }
}

void Engine::renameSequenceFile (const juce::File& from, const juce::File& to)
{
    const juce::ScopedLock sl (tracksLock);
    auto it = sequenceCache.find (from.getFullPathName());
    if (it == sequenceCache.end())
        return;
    auto seq = it->second;
    sequenceCache.erase (it);
    seq->file = to;
    seq->name = to.getFileNameWithoutExtension();
    sequenceCache[to.getFullPathName()] = seq;
}

void Engine::relocateSequences (const juce::File& oldDir, const juce::File& newDir)
{
    const juce::ScopedLock sl (tracksLock);
    std::map<juce::String, std::shared_ptr<Sequence>> moved;
    for (auto& kv : sequenceCache)
    {
        auto seq = kv.second;
        if (seq->file.isAChildOf (oldDir))
            seq->file = newDir.getChildFile (seq->file.getRelativePathFrom (oldDir));
        moved[seq->file.getFullPathName()] = seq;
    }
    sequenceCache.swap (moved);
}
