#pragma once

#include <juce_core/juce_core.h>
#include <vector>

double detectTempo (const std::vector<float>& mono, double sampleRate);
