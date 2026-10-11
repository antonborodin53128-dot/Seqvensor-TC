#include "Engine.h"

Engine::Engine()
{
    audioFormats.registerBasicFormats();
    pluginFormats.addFormat (std::make_unique<juce::VST3PluginFormat>());
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
    readThread.startThread (juce::Thread::Priority::normal);

    // saved plugin list
    auto listFile = getDataDirectory().getChildFile ("plugins.xml");
    if (auto xml = juce::XmlDocument::parse (listFile))
        knownPlugins.recreateFromXml (*xml);

    // saved audio setup
    std::unique_ptr<juce::XmlElement> audioState = juce::XmlDocument::parse (getDataDirectory().getChildFile ("audio.xml"));
    const auto error = deviceManager.initialise (64, 64, audioState.get(), true);
    if (error.isNotEmpty())
        juce::Logger::writeToLog ("Audio init: " + error);

    deviceManager.addAudioCallback (this);
}

void Engine::shutdown()
{
    if (! readThread.isThreadRunning() && ! running && tracks.empty())
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
    readThread.stopThread (2000);
}

void Engine::savePluginList() const
{
    if (auto xml = knownPlugins.createXml())
        xml->writeTo (getDataDirectory().getChildFile ("plugins.xml"));
}

// ---------------------------------------------------------------- tracks

std::shared_ptr<Track> Engine::addTrack (Track::Kind kind, const juce::String& name)
{
    auto track = std::make_shared<Track> (kind, name, audioFormats, readThread);
    track->prepare (currentSampleRate, currentBlockSize, &playHead);
    {
        const juce::ScopedLock sl (tracksLock);
        tracks.push_back (track);
    }
    if (onTracksChanged)
        onTracksChanged();
    return track;
}

void Engine::removeTrack (const std::shared_ptr<Track>& track)
{
    {
        const juce::ScopedLock sl (tracksLock);
        tracks.erase (std::remove (tracks.begin(), tracks.end(), track), tracks.end());
    }
    if (onTracksChanged)
        onTracksChanged();
}

std::vector<std::shared_ptr<Track>> Engine::getTracks() const
{
    const juce::ScopedLock sl (tracksLock);
    return tracks;
}

void Engine::addPluginToTrack (const std::shared_ptr<Track>& track, const juce::PluginDescription& description,
                               std::function<void (bool, const juce::String&)> done,
                               const juce::String& savedStateBase64, bool bypassed)
{
    std::weak_ptr<Track> weak = track;
    pluginFormats.createPluginInstanceAsync (
        description, currentSampleRate, currentBlockSize,
        [this, weak, description, done, savedStateBase64, bypassed]
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

            trackPtr->addPlugin (std::move (instance), description);
            if (auto* slot = trackPtr->getSlot (trackPtr->getNumPlugins() - 1))
                slot->bypass.store (bypassed);
            trackPtr->refreshLatency();

            if (done)
                done (true, {});
        });
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
            return juce::String::formatted ("буфер %.1f мс, вход %.1f мс, выход %.1f мс"_ru, buf, in, out);
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

    bool anyPlaying = false;
    for (auto& t : tracks)
    {
        t->process (inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
        if (t->kind == Track::Kind::Player && t->isPlaying())
            anyPlaying = true;
    }

    playHead.playing.store (anyPlaying);
    playHead.advance (numSamples);
}

// ---------------------------------------------------------------- project file

void Engine::saveProject()
{
    juce::XmlElement root ("TC_PROJECT");
    root.setAttribute ("bpm", getBpm());

    for (auto& t : getTracks())
    {
        auto* te = root.createNewChildElement ("TRACK");
        te->setAttribute ("kind", t->kind == Track::Kind::Live ? "live" : "player");
        te->setAttribute ("name", t->name);
        te->setAttribute ("inL", t->inputL.load());
        te->setAttribute ("inR", t->inputR.load());
        te->setAttribute ("out", t->outputPair.load());
        te->setAttribute ("gain", (double) t->gainLinear.load());
        te->setAttribute ("mute", t->muted.load());
        te->setAttribute ("comp", t->compensate.load());
        te->setAttribute ("loop", t->isLooping());
        if (t->getFile() != juce::File())
            te->setAttribute ("file", t->getFile().getFullPathName());

        for (int i = 0; i < t->getNumPlugins(); ++i)
        {
            auto* slot = t->getSlot (i);
            if (slot == nullptr || slot->plugin == nullptr)
                continue;

            auto* pe = te->createNewChildElement ("PLUGIN");
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
        const auto kind = te->getStringAttribute ("kind") == "player" ? Track::Kind::Player : Track::Kind::Live;
        auto track = addTrack (kind, te->getStringAttribute ("name", "Дорожка"_ru));

        track->inputL = te->getIntAttribute ("inL", 0);
        track->inputR = te->getIntAttribute ("inR", -1);
        track->outputPair = te->getIntAttribute ("out", 0);
        track->gainLinear = (float) te->getDoubleAttribute ("gain", 1.0);
        track->muted = te->getBoolAttribute ("mute", false);
        track->compensate = te->getBoolAttribute ("comp", false);
        track->setLooping (te->getBoolAttribute ("loop", false));

        if (kind == Track::Kind::Player && te->hasAttribute ("file"))
        {
            juce::File f (te->getStringAttribute ("file"));
            if (f.existsAsFile())
                track->loadFile (f);
        }

        for (auto* pe : te->getChildWithTagNameIterator ("PLUGIN"))
        {
            juce::PluginDescription desc;
            if (auto* de = pe->getFirstChildElement())
                if (desc.loadFromXml (*de))
                    addPluginToTrack (track, desc,
                                      [this] (bool, const juce::String&) { if (onTracksChanged) onTracksChanged(); },
                                      pe->getStringAttribute ("state"), pe->getBoolAttribute ("bypass", false));
        }
    }

    if (onTracksChanged)
        onTracksChanged();
}
