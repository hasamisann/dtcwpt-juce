#include "GateGuiFrequencyMapping.h"

#include <algorithm>
#include <cmath>

namespace gate_gui
{
    SanitizedSampleRate sanitizeSampleRateAndNyquist (float sampleRate) noexcept
    {
        const float safeSampleRate = sampleRate > 0.0f ? sampleRate : kFallbackSampleRate;
        return SanitizedSampleRate { safeSampleRate, safeSampleRate * 0.5f };
    }

    FrequencyMapping makeFrequencyMapping (float nyquist) noexcept
    {
        const float sanitizedNyquist = nyquist > 0.0f
                                          ? nyquist
                                          : sanitizeSampleRateAndNyquist (0.0f).nyquist;
        const float displayNyquist = std::max (sanitizedNyquist, kFreqMinDisplay * 2.0f);
        return FrequencyMapping { displayNyquist, std::log10 (kFreqMinDisplay), std::log10 (displayNyquist) };
    }

    std::pair<float, float> getBandFrequencyRange (const std::string& path, float nyquist) noexcept
    {
        float lo = 0.0f;
        float hi = nyquist > 0.0f ? nyquist : sanitizeSampleRateAndNyquist (0.0f).nyquist;

        for (const char ch : path)
        {
            const float mid = (lo + hi) * 0.5f;
            if (ch == 'L')
                hi = mid;
            else
                lo = mid;
        }

        return std::pair<float, float> { lo, hi };
    }

    std::vector<FrequencyRange> buildFrequencyRangesForTopology (const std::vector<std::string>& destinations,
                                                                 float sampleRate)
    {
        const auto sanitized = sanitizeSampleRateAndNyquist (sampleRate);

        std::vector<FrequencyRange> ranges;
        ranges.push_back (FrequencyRange { "", 1, 0.0f, sanitized.nyquist, false });

        for (const auto& destination : destinations)
        {
            for (std::size_t i = 0; i < destination.size(); ++i)
            {
                const std::string path = destination.substr (0, i + 1U);
                const auto existing = std::find_if (ranges.begin(), ranges.end(), [&path] (const FrequencyRange& range)
                {
                    return range.path == path;
                });

                if (existing != ranges.end())
                    continue;

                int nodeId = 1;
                for (const char ch : path)
                    nodeId = (ch == 'L') ? (2 * nodeId) : (2 * nodeId + 1);

                const auto [freqLow, freqHigh] = getBandFrequencyRange (path, sanitized.nyquist);
                ranges.push_back (FrequencyRange { path, nodeId, freqLow, freqHigh, i + 1U == destination.size() });
            }
        }

        return ranges;
    }
}
