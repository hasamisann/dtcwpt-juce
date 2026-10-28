#pragma once

#include "../GateThresholdCurve.h"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace gate_gui
{
    inline constexpr float kFallbackSampleRate = 44100.0f;
    inline constexpr float kFreqMinDisplay = 20.0f;
    inline constexpr float kDbMinDisplay = -120.0f;
    inline constexpr float kDbMaxDisplay = 10.0f;

    struct SanitizedSampleRate
    {
        float sampleRate { kFallbackSampleRate };
        float nyquist { kFallbackSampleRate * 0.5f };
    };

    struct FrequencyMapping
    {
        float displayNyquist { kFallbackSampleRate * 0.5f };
        float logMin { std::log10 (kFreqMinDisplay) };
        float logMax { std::log10 (kFallbackSampleRate * 0.5f) };
    };

    struct FrequencyRange
    {
        std::string path;
        int nodeId { 1 };
        float freqLow { 0.0f };
        float freqHigh { kFallbackSampleRate * 0.5f };
        bool isLeaf { false };
    };

    SanitizedSampleRate sanitizeSampleRateAndNyquist (float sampleRate) noexcept;

    FrequencyMapping makeFrequencyMapping (float nyquist) noexcept;

    std::pair<float, float> getBandFrequencyRange (const std::string& path, float nyquist) noexcept;

    /** Number of threshold points (ISO 1/3 octave) at or below the display Nyquist; these are the ones the graph shows. */
    int countThresholdPointsBelow (float nyquist) noexcept;

    /** Threshold point nearest to @p frequencyHz on the log frequency axis, among the points shown for @p nyquist. */
    int findNearestThresholdPoint (float frequencyHz, float nyquist) noexcept;

    std::vector<FrequencyRange> buildFrequencyRangesForTopology (const std::vector<std::string>& destinations,
                                                                 float sampleRate);
}
