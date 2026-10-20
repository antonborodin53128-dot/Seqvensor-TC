#pragma once

// Keeps Windows from slowing TC down: on laptops (and every Windows on ARM) a window that is not in front, or a
// machine on battery, gets "power throttling" (efficiency cores, coalesced timers), and the audio crackles.
namespace tcrt
{
    void boostProcess();       // once at start: high priority class, no power throttling for the process
    void boostAudioThread();   // from the audio callback: MMCSS "Pro Audio" + no throttling, done once per thread
}
