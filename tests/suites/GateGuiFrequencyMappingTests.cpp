#include "../../sample_plugins/dtcwpt-gate/gui/GateGuiFrequencyMapping.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

class GateGuiFrequencyMappingTests : public juce::UnitTest
{
public:
    GateGuiFrequencyMappingTests() : juce::UnitTest ("GateGuiFrequencyMappingTests") {}

    void runTest() override
    {
        beginTest ("zero sample rate sanitizes to positive fallback nyquist");
        {
            const auto sanitized = gate_gui::sanitizeSampleRateAndNyquist (0.0f);

            expectGreaterThan (sanitized.sampleRate, 0.0f);
            expectGreaterThan (sanitized.nyquist, 0.0f);
        }

        beginTest ("negative sample rate sanitizes to positive fallback nyquist");
        {
            const auto sanitized = gate_gui::sanitizeSampleRateAndNyquist (-48000.0f);

            expectGreaterThan (sanitized.sampleRate, 0.0f);
            expectGreaterThan (sanitized.nyquist, 0.0f);
        }

        beginTest ("valid sample rate preserves expected nyquist");
        {
            const auto sanitized = gate_gui::sanitizeSampleRateAndNyquist (48000.0f);

            expectWithinAbsoluteError (sanitized.sampleRate, 48000.0f, 1.0e-4f);
            expectWithinAbsoluteError (sanitized.nyquist, 24000.0f, 1.0e-4f);
        }

        beginTest ("graphic eq mapping metadata stays logarithm-safe for invalid nyquist inputs");
        {
            const auto mapping = gate_gui::makeFrequencyMapping (0.0f);

            expectGreaterThan (mapping.displayNyquist, 0.0f);
            expectGreaterThan (mapping.logMax, mapping.logMin);
            expect (std::isfinite (mapping.logMin));
            expect (std::isfinite (mapping.logMax));
        }

        beginTest ("topology ranges stay ordered for root and descendants with invalid sample rate");
        {
            const auto ranges = gate_gui::buildFrequencyRangesForTopology (
                { "H", "LH", "LLH", "LLLL" },
                0.0);

            expectGreaterThan (static_cast<int> (ranges.size()), 0);

            for (const auto& range : ranges)
            {
                expectGreaterThan (range.freqHigh, range.freqLow);
                expect (std::isfinite (range.freqLow));
                expect (std::isfinite (range.freqHigh));
            }
        }

        beginTest ("depth-3 packet paths map to the frequency bands measured in issue #12");
        {
            // 48 kHz: each band is 3 kHz wide; H descendants are in Gray-code order.
            const std::vector<std::pair<std::string, std::pair<float, float>>> expected {
                { "LLL", { 0.0f, 3000.0f } },     { "LLH", { 3000.0f, 6000.0f } },
                { "LHH", { 6000.0f, 9000.0f } },  { "LHL", { 9000.0f, 12000.0f } },
                { "HHL", { 12000.0f, 15000.0f } }, { "HHH", { 15000.0f, 18000.0f } },
                { "HLH", { 18000.0f, 21000.0f } }, { "HLL", { 21000.0f, 24000.0f } } };

            for (const auto& [path, range] : expected)
            {
                const auto actual = gate_gui::getBandFrequencyRange (path, 24000.0f);
                expectWithinAbsoluteError (actual.first, range.first, 1.0e-3f, juce::String (path) + " low edge");
                expectWithinAbsoluteError (actual.second, range.second, 1.0e-3f, juce::String (path) + " high edge");
            }
        }

        beginTest ("topology ranges carry the Gray-code frequency bands");
        {
            const auto ranges = gate_gui::buildFrequencyRangesForTopology (
                { "LLL", "LLH", "LHL", "LHH", "HLL", "HLH", "HHL", "HHH" }, 48000.0);

            const auto it = std::find_if (ranges.begin(), ranges.end(),
                                          [] (const gate_gui::FrequencyRange& r) { return r.path == "HLL"; });
            expect (it != ranges.end());
            if (it != ranges.end())
            {
                expectWithinAbsoluteError (it->freqLow, 21000.0f, 1.0e-3f);
                expectWithinAbsoluteError (it->freqHigh, 24000.0f, 1.0e-3f);
            }
        }
    }
};

static GateGuiFrequencyMappingTests gateGuiFrequencyMappingTests;
