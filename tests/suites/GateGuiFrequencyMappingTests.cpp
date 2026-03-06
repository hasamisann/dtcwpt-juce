#include "../../sample_plugins/dtcwpt-gate/gui/GateGuiFrequencyMapping.h"

#include <juce_core/juce_core.h>

#include <cmath>

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
    }
};

static GateGuiFrequencyMappingTests gateGuiFrequencyMappingTests;
