#pragma once

#include "Clip.h"

#include <functional>

// Offline time-stretch that keeps the pitch (Signalsmith Stretch). timeRatio = new length / old length,
// so 0.9 plays faster and 1.1 slower. Call from a background thread. Returns null when `cancelled()` turns true.
std::shared_ptr<ClipData> stretchClipData (const ClipData& source, double timeRatio,
                                           const std::function<bool()>& cancelled);
