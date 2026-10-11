#pragma once

#include "Engine.h"
#include "TrackComponent.h"

// Window of the built-in "TC Player" plugin: add an audio file, see its waveform on the
// metronome grid, drag it left/right to align it, mouse wheel = zoom.
class PlayerWindow : public juce::DocumentWindow
{
public:
    PlayerWindow (Engine& engine, std::shared_ptr<Track> track, std::function<void (PlayerWindow*)> onCloseRequested);

    void closeButtonPressed() override { if (onClose) onClose (this); }
    Track* getTrack() const { return track.get(); }

private:
    std::shared_ptr<Track> track;
    std::function<void (PlayerWindow*)> onClose;
};
