#pragma once

#include <juce_core/juce_core.h>

// JUCE treats plain "..." literals as ASCII. Anything with Cyrillic or symbols must go through UTF-8:
//     "Привет"_ru
inline juce::String operator""_ru (const char* text, std::size_t length)
{
    return juce::String::fromUTF8 (text, (int) length);
}
