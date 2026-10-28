/**
 * @file OversizedBlockHelpers.h
 * @brief Shared helpers for the Morph/Gate oversized-host-block regression tests.
 */

#pragma once

#include "TestUtils.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace OversizedBlockHelpers {

constexpr int kPreparedBlockSize = 256;
constexpr int kTotalSamples = 4096;

using Signal = std::vector<std::vector<float>>; // [channel][sample]

inline Signal makeSignal (int numChannels, int numSamples)
{
    Signal signal (static_cast<std::size_t> (numChannels));
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto& channel = signal[static_cast<std::size_t> (ch)];
        channel.resize (static_cast<std::size_t> (numSamples));
        const double frequency = 330.0 * (ch + 1);
        for (int i = 0; i < numSamples; ++i)
        {
            const double t = static_cast<double> (i) / TestUtils::TestConfig::SAMPLE_RATE;
            channel[static_cast<std::size_t> (i)] =
                static_cast<float> (0.4 * std::sin (juce::MathConstants<double>::twoPi * frequency * t)
                                    + 0.1 * std::cos (juce::MathConstants<double>::twoPi * 3.1 * frequency * t));
        }
    }
    return signal;
}

/** Feeds `signal` through the processor with the cyclic block-length `pattern`; returns the two main output channels as one continuous stream each (channel 0 then channel 1). */
template <typename Processor>
std::vector<float> run (Processor& processor, const Signal& signal, const std::vector<int>& pattern)
{
    const int numChannels = static_cast<int> (signal.size());
    const int totalSamples = static_cast<int> (signal.front().size());

    std::vector<float> streams (static_cast<std::size_t> (2 * totalSamples));
    juce::MidiBuffer midi;

    int cursor = 0;
    std::size_t patternIndex = 0;
    while (cursor < totalSamples)
    {
        const int length = std::min (pattern[patternIndex++ % pattern.size()], totalSamples - cursor);

        juce::AudioBuffer<float> buffer (numChannels, length);
        for (int ch = 0; ch < numChannels; ++ch)
            std::copy_n (signal[static_cast<std::size_t> (ch)].begin() + cursor, length, buffer.getWritePointer (ch));

        processor.processBlock (buffer, midi);

        for (int ch = 0; ch < 2; ++ch)
            std::copy_n (buffer.getReadPointer (ch), length,
                         streams.begin() + static_cast<std::ptrdiff_t> (ch * totalSamples + cursor));

        cursor += length;
    }

    return streams;
}

/** Block-length patterns a host may use after promising a maximum of kPreparedBlockSize. */
inline const std::vector<std::vector<int>>& getHostilePatterns()
{
    static const std::vector<std::vector<int>> patterns = {
        { 300 }, { 1024 }, { 1, 300, 1024, 7, 255, 513 }, { 1 }
    };
    return patterns;
}

} // namespace OversizedBlockHelpers
