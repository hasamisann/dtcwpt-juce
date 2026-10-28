/**
 * @file GateBypassTests.cpp
 * @brief Bypass / latency-compensation tests for GateAudioProcessor.
 */

#include "GateAudioProcessor.h"

#include "../util/BypassTestSuite.h"

namespace
{
    struct GateBypassTraits
    {
        static const char* suiteName() { return "GateBypassTests"; }

        static constexpr int kNumInputChannels = 2;
        static constexpr int kDeepTopologyDepth = 8;
        static constexpr float kSineAmplitude = 0.2f;

        /** Thresholds above any subband magnitude close every band, so the live path is silent. */
        static void configureSilentWet (GateAudioProcessor& processor)
        {
            for (int band = 0; band < GateBandProcessor::kMaxBands; ++band)
            {
                const auto id = juce::String::formatted ("threshold_%02d", band);
                auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (processor.getAPVTS().getParameter (id));
                jassert (parameter != nullptr);
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (10.0f));
            }
        }
    };

    BypassTestSuite<GateAudioProcessor, GateBypassTraits> gateBypassTests;
} // namespace
