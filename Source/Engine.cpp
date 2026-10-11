#include "Engine.h"

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

    // saved audio setup
    std::unique_ptr<juce::XmlElement> audioState = juce::XmlDocument::parse (getDataDirectory().getChildFile ("audio.xml"));
    const auto error = deviceManager.initialise (64, 64, audioState.get(), true);
    if (error.isNotEmpty())
        juce::Logger::writeToLog ("Audio init: " + error);

    deviceManager.addAudioCallback (this);
}

void Engine::shutdown()
{
    if (! running && tracks.empty())
        return;

    saveProject();

    if (auto xml = deviceManager.createStateXml())
        xml->writeTo (getDataDirectory().getChildFile ("audio.xml"));

    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();

    {
        const juce::ScopedLock sl (tracksLock);
        tracks.clear();
    }
}

void Engine::savePluginList() const
{
    if (auto xml = knownPlugins.createXml())
        xml->writeTo (getDataDirectory().getChildFile ("plugins.xml"));
}

// ---------------------------------------------------------------- tracks

std::shared_ptr<Track> Engine::addTrack (const juce::String& name, int forcedId)
{
    auto track = std::make_shared<Track> (name);
    track->id = forcedId > 0 ? forcedId : nextTrackId;
    nextTrackId = juce::jmax (nextTrackId, track->id + 1);
    track->setTempoSource (&playHead.bpm);
    track->prepare (currentSampleRate, currentBlockSize, &playHead);
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

void Engine::playAll()
{
    if (clockRunning.load() || command.load() == 1)
        return;   // already playing

    double start = 0.0;
    if (paused.load())
    {
        start = clockNow.load();   // resume where we paused
    }
    else
    {
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

void Engine::pauseAll()
{
    if (! (clockRunning.load() || command.load() == 1))
        return;

    command.store (0);
    clockRunning.store (false);
    paused.store (true);
}

void Engine::stopAll()
{
    command.store (0);
    clockRunning.store (false);
    paused.store (false);
    clockNow.store (0.0);
}

void Engine::seek (double clockSecondsTarget)
{
    // from 4 beats before bar 1 (the count-in) to a little after the end of the song
    const double t = juce::jlimit (-4.0 * getBeatSeconds(), juce::jmax (60.0, songEnd.load() + 30.0), clockSecondsTarget);

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

    const juce::ScopedLock sl (tracksLock);
    for (auto& t : tracks)
        t->prepare (currentSampleRate, currentBlockSize, &playHead);
    running = true;
}

void Engine::audioDeviceStopped()
{
    const juce::ScopedLock sl (tracksLock);
    for (auto& t : tracks)
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
    const juce::AudioProcessLoadMeasurer::ScopedTimer timer (loadMeasure, numSamples);

    for (int c = 0; c < numOutputChannels; ++c)
        if (outputChannelData[c] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[c], numSamples);

    const juce::ScopedTryLock lock (tracksLock);
    if (! lock.isLocked())
        return;

    updateCompensation();

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

void Engine::addClipFromFile (const std::shared_ptr<Track>& track, const juce::File& file, double startSeconds,
                              std::function<void (std::shared_ptr<Clip>, const juce::String&)> done)
{
    track->loadingClips++;
    std::weak_ptr<Track> weak = track;

    juce::Thread::launch ([this, weak, file, startSeconds, done]
    {
        juce::String error;
        auto data = ClipData::load (file, error);

        juce::MessageManager::callAsync ([this, weak, data, error, startSeconds, done]
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
                clip = t->addClip (data, startSeconds, takeClipId());
                clipsChanged();
            }
            if (done)
                done (clip, error);
        });
    });
}

void Engine::removeClip (const std::shared_ptr<Track>& track, int clipId)
{
    track->removeClip (clipId);
    clipsChanged();
}

// ---------------------------------------------------------------- project file

void Engine::saveProject()
{
    juce::XmlElement root ("TC_PROJECT");
    root.setAttribute ("version", 3);
    root.setAttribute ("bpm", getBpm());

    for (auto& t : getTracks())
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
        te->setAttribute ("rec", t->recArmed.load());
        te->setAttribute ("comp", t->compensate.load());
        te->setAttribute ("metro", t->metronomeOn.load());
        te->setAttribute ("inputSet", t->inputChosenByUser);

        for (auto& c : t->getClips())
        {
            auto* ce = te->createNewChildElement ("CLIP");
            ce->setAttribute ("file", c->data->file.getFullPathName());
            ce->setAttribute ("start", c->start.load());
            ce->setAttribute ("loop", c->loop.load());
        }

        for (int i = 0; i < Track::numSlots; ++i)
        {
            auto* slot = t->getSlot (i);
            if (slot == nullptr)
                continue;

            if (slot->plugin == nullptr)
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

    getProjectFile().replaceWithText (root.toString());
}

void Engine::loadProject()
{
    auto xml = juce::XmlDocument::parse (getProjectFile());
    if (xml == nullptr || ! xml->hasTagName ("TC_PROJECT"))
        return;

    setBpm (xml->getDoubleAttribute ("bpm", 120.0));

    for (auto* te : xml->getChildWithTagNameIterator ("TRACK"))
    {
        auto track = addTrack (te->getStringAttribute ("name", "Дорожка"_ru), te->getIntAttribute ("id", -1));

        track->inputL = te->getIntAttribute ("inL", -1);
        track->inputR = te->getIntAttribute ("inR", -1);
        track->busId = te->getIntAttribute ("bus", -1);
        track->outputPair = te->getIntAttribute ("out", 0);
        track->gainLinear = (float) te->getDoubleAttribute ("gain", 1.0);
        track->muted = te->getBoolAttribute ("mute", false);
        track->recArmed = te->getBoolAttribute ("rec", false);
        track->compensate = te->getBoolAttribute ("comp", false);
        track->inputChosenByUser = te->getBoolAttribute ("inputSet", true);
        if (xml->getIntAttribute ("version", 1) >= 3)
            track->metronomeOn = te->getBoolAttribute ("metro", false);

        for (auto* ce : te->getChildWithTagNameIterator ("CLIP"))
        {
            juce::File f (ce->getStringAttribute ("file"));
            if (! f.existsAsFile())
                continue;
            addClipFromFile (track, f, ce->getDoubleAttribute ("start", 0.0),
                             [this, loop = ce->getBoolAttribute ("loop", false)] (std::shared_ptr<Clip> clip, const juce::String&)
                             {
                                 if (clip != nullptr)
                                     clip->loop = loop;
                                 clipsChanged();
                                 if (onTracksChanged) onTracksChanged();
                             });
        }

        // projects from the first versions: a built-in player with a file and an offset
        for (auto* pe : te->getChildWithTagNameIterator ("PLAYER"))
        {
            juce::ignoreUnused (pe);
            juce::File f (te->getStringAttribute ("file"));
            if (f.existsAsFile())
                addClipFromFile (track, f, te->getDoubleAttribute ("offset", 0.0));
        }

        int fallbackSlot = 0;
        for (auto* pe : te->getChildWithTagNameIterator ("PLUGIN"))
        {
            const int slot = pe->hasAttribute ("slot") ? pe->getIntAttribute ("slot") : fallbackSlot++;
            juce::PluginDescription desc;
            if (auto* de = pe->getFirstChildElement())
                if (desc.loadFromXml (*de))
                    addPluginToTrack (track, slot, desc,
                                      [this] (bool, const juce::String&) { if (onTracksChanged) onTracksChanged(); },
                                      pe->getStringAttribute ("state"), pe->getBoolAttribute ("bypass", false));
        }
    }

    if (onTracksChanged)
        onTracksChanged();
}
