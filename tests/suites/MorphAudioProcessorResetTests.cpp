/**
 * @file MorphAudioProcessorResetTests.cpp
 * @brief JUCE UnitTest suite for MorphAudioProcessor::reset().
 */

#include "MorphAudioProcessor.h"

#include "../util/TestUtils.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

class MorphAudioProcessorResetTests : public juce::UnitTest
{
public:
    MorphAudioProcessorResetTests() : UnitTest ("MorphAudioProcessorResetTests") {}

    void runTest() override
    {
        beginTest ("Without reset, silence still flushes previously processed audio");
        testStaleAudioLeaksWithoutReset();

        beginTest ("reset() turns silent input into exact silence");
        testResetSilencesOutput();

        beginTest ("reset() output is bit-identical to a settled fresh processor");
        testResetMatchesSettledFreshProcessor();

        beginTest ("reset() clears the analyzer latency aligner");
        testResetClearsAnalyzerAligner();

        beginTest ("reset() is allocation-free");
        testResetIsAllocationFree();
    }

private:
    static constexpr int kBlockSize = 512;

    /** Channels 0/1 = main L/R, 2/3 = sidechain L/R. */
    using Planar = std::array<std::vector<float>, 4>;
    using StereoOut = std::array<std::vector<float>, 2>;

    static Planar makeNoise (int numSamples, unsigned int seed)
    {
        Planar signal;
        for (std::size_t ch = 0; ch < signal.size(); ++ch)
        {
            const auto noise = TestUtils::generateNoise (
                static_cast<double> (numSamples) / TestUtils::TestConfig::SAMPLE_RATE,
                TestUtils::TestConfig::SAMPLE_RATE,
                seed + static_cast<unsigned int> (ch));

            signal[ch].assign (static_cast<std::size_t> (numSamples), 0.0f);
            for (std::size_t i = 0; i < signal[ch].size() && i < noise.size(); ++i)
                signal[ch][i] = static_cast<float> (noise[i]);
        }
        return signal;
    }

    static Planar makeSilence (int numSamples)
    {
        Planar signal;
        for (auto& channel : signal)
            channel.assign (static_cast<std::size_t> (numSamples), 0.0f);
        return signal;
    }

    static void setFloatParameter (MorphAudioProcessor& processor, const char* parameterId, float value)
    {
        auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (processor.getAPVTS().getParameter (parameterId));
        jassert (parameter != nullptr);
        if (parameter != nullptr)
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    static void setBoolParameter (MorphAudioProcessor& processor, const char* parameterId, bool value)
    {
        if (auto* parameter = processor.getAPVTS().getParameter (parameterId))
            parameter->setValueNotifyingHost (value ? 1.0f : 0.0f);
    }

    static void applyParameters (MorphAudioProcessor& processor, float phase)
    {
        setFloatParameter (processor, ParamID::Magnitude, 0.8f);
        setFloatParameter (processor, ParamID::Phase, phase);
        setFloatParameter (processor, ParamID::Threshold, -40.0f);
        setBoolParameter (processor, ParamID::BypassLow, false);
        setBoolParameter (processor, ParamID::BypassHigh, false);
    }

    static std::unique_ptr<MorphAudioProcessor> createProcessor (float phase)
    {
        auto processor = std::make_unique<MorphAudioProcessor> ();
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);
        applyParameters (*processor, phase);
        return processor;
    }

    static StereoOut process (MorphAudioProcessor& processor, const Planar& input)
    {
        StereoOut output;
        const int totalSamples = static_cast<int> (input[0].size());

        for (int cursor = 0; cursor < totalSamples; cursor += kBlockSize)
        {
            const int blockSize = std::min (kBlockSize, totalSamples - cursor);
            juce::AudioBuffer<float> buffer (4, blockSize);
            for (int ch = 0; ch < 4; ++ch)
                std::copy_n (input[static_cast<std::size_t> (ch)].begin() + cursor, blockSize,
                             buffer.getWritePointer (ch));

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
            {
                const float* out = buffer.getReadPointer (ch);
                output[static_cast<std::size_t> (ch)].insert (output[static_cast<std::size_t> (ch)].end(),
                                                              out, out + blockSize);
            }
        }

        return output;
    }

    static float maxAbs (const StereoOut& signal)
    {
        float peak = 0.0f;
        for (const auto& channel : signal)
            for (const float sample : channel)
                peak = std::max (peak, std::abs (sample));
        return peak;
    }

    //==========================================================================

    void testStaleAudioLeaksWithoutReset()
    {
        auto processor = createProcessor (0.7f);
        (void) process (*processor, makeNoise (kBlockSize * 16, 3U));

        const auto tail = process (*processor, makeSilence (kBlockSize * 6));
        expect (maxAbs (tail) > 0.0f,
                "Without reset(), old audio is still emitted when silence arrives");
    }

    void testResetSilencesOutput()
    {
        auto processor = createProcessor (0.7f);
        const auto pre = process (*processor, makeNoise (kBlockSize * 16, 5U));
        expect (maxAbs (pre) > 0.0f, "Noise produced output before reset");

        processor->reset();

        const auto post = process (*processor, makeSilence (kBlockSize * 8));
        expect (maxAbs (post) <= 0.0f,
                "Silence after AudioProcessor::reset() must give exact zeros, peak = "
                + juce::String (maxAbs (post)));
    }

    void testResetMatchesSettledFreshProcessor()
    {
        // The reference is a freshly prepared processor whose parameter smoothers
        // have been given time to settle on the same targets: reset() snaps the
        // smoothers to their targets, so both start the comparison from the same state.
        auto resetProcessor = createProcessor (0.7f);
        (void) process (*resetProcessor, makeNoise (kBlockSize * 16, 7U));

        // Leave the phase smoother in the middle of a ramp, then reset.
        setFloatParameter (*resetProcessor, ParamID::Phase, 0.3f);
        (void) process (*resetProcessor, makeNoise (kBlockSize, 8U));
        resetProcessor->reset();

        auto freshProcessor = createProcessor (0.3f);
        (void) process (*freshProcessor, makeSilence (kBlockSize * 4));

        const auto input = makeNoise (kBlockSize * 12, 9U);
        const auto afterReset = process (*resetProcessor, input);
        const auto fromFresh = process (*freshProcessor, input);

        expect (maxAbs (fromFresh) > 0.0f, "Reference output is not silent");

        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            std::size_t mismatches = 0;
            std::size_t firstMismatch = 0;
            for (std::size_t i = 0; i < fromFresh[ch].size(); ++i)
            {
                if (std::not_equal_to<> {} (afterReset[ch][i], fromFresh[ch][i]))
                {
                    if (mismatches == 0)
                        firstMismatch = i;
                    ++mismatches;
                }
            }

            expect (mismatches == 0,
                    "Channel " + juce::String (static_cast<int> (ch)) + " has "
                    + juce::String (static_cast<int> (mismatches))
                    + " samples that differ from a fresh processor (first at "
                    + juce::String (static_cast<int> (firstMismatch)) + ")");
        }
    }

    void testResetClearsAnalyzerAligner()
    {
        // Exactly one spectrum slot of noise, then reset, then exactly one slot of
        // silence: the aligned analyzer input of the second slot must be silent.
        constexpr int kSlot = SpectrumDataBridge::kSlotSize;
        static_assert (kSlot % kBlockSize == 0, "Slot must be a whole number of blocks");

        auto processor = createProcessor (0.7f);
        (void) process (*processor, makeNoise (kSlot, 13U));

        processor->reset();
        (void) process (*processor, makeSilence (kSlot));

        std::array<float, kSlot> input {};
        std::array<float, kSlot> output {};
        const bool available = processor->consumeSpectrumSnapshotForTest (input, output);
        expect (available, "A completed spectrum slot should be available");

        const float inputPeak = *std::max_element (input.begin(), input.end(),
                                                   [] (float a, float b) { return std::abs (a) < std::abs (b); });
        expect (std::abs (inputPeak) <= 0.0f,
                "Aligned analyzer input after reset() must not contain pre-reset audio, peak = "
                + juce::String (std::abs (inputPeak)));
    }

    void testResetIsAllocationFree()
    {
        auto processor = createProcessor (0.7f);
        (void) process (*processor, makeNoise (kBlockSize * 4, 17U));

        TestUtils::AllocationCounter counter;
        counter.begin();
        processor->reset();
        counter.end();

        expect (counter.getCount() == 0,
                "AudioProcessor::reset() must not allocate - found "
                + juce::String (static_cast<int> (counter.getCount())));
    }
};

static MorphAudioProcessorResetTests morphAudioProcessorResetTests;
