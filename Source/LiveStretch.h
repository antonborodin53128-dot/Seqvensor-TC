#pragma once

#include "Clip.h"

#include <atomic>
#include <cmath>
#include <memory>

// Real-time pitch-preserving time-stretch of one clip (Signalsmith Stretch), used while a clip follows the
// project tempo. The file position is always derived from the song position in bars, so the audio can never
// drift away from the metronome, whatever the tempo does.
class LiveStretch
{
public:
    // The audio is taken at the device rate; a file with another rate is resampled once (buildSource).
    LiveStretch (std::shared_ptr<const ClipData> original, double deviceRate, bool buildSource);
    ~LiveStretch();

    bool isReady (double deviceRate) const { return ready.load() && std::abs (rate - deviceRate) < 0.5; }

    // Audio thread. Adds n stretched samples to outL / outR. posStart = position in the file (in samples at the
    // device rate) of the first output sample, rho = file samples consumed per output sample (1 = original tempo),
    // 0.25 <= rho <= 4.
    void render (float* outL, float* outR, int n, double posStart, double rho, bool loop);

    // The clip is not sounding right now: the next render starts from a fresh position.
    void idle() { primed = false; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    double rate = 48000.0;
    std::atomic<bool> ready { false };
    bool primed = false;

    void renderPiece (float* outL, float* outR, int n, double posStart, double rho, bool loop);
    void reseek (double posStart, double rho, bool loop);
};
