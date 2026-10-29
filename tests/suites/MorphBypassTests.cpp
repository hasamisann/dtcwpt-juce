/**
 * @file MorphBypassTests.cpp
 * @brief Bypass / latency-compensation tests for MorphAudioProcessor.
 */

#include "MorphAudioProcessor.h"

#include "../util/BypassTestSuite.h"

namespace
{
    struct MorphBypassTraits
    {
        static const char* suiteName() { return "MorphBypassTests"; }

        // Stereo main + stereo sidechain (kept silent).
        static constexpr int kNumInputChannels = 4;
        static constexpr int kDeepTopologyDepth = 12;
        static constexpr float kSineAmplitude = 0.5f;

        /** Magnitude 1 takes the magnitude from the (silent) sidechain, so the live path is silent. */
        static void configureSilentWet (MorphAudioProcessor& processor)
        {
            auto setFloat = [&processor] (const char* id, float value)
            {
                auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (processor.getAPVTS().getParameter (id));
                jassert (parameter != nullptr);
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
            };

            setFloat (ParamID::Magnitude, 1.0f);
            setFloat (ParamID::BypassLow, 0.0f);
            setFloat (ParamID::BypassHigh, 0.0f);
        }
    };

    BypassTestSuite<MorphAudioProcessor, MorphBypassTraits> morphBypassTests;
} // namespace
