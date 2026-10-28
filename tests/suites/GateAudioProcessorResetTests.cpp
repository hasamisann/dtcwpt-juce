/**
 * @file GateAudioProcessorResetTests.cpp
 * @brief JUCE UnitTest suite for GateAudioProcessor::reset() and BandLevelBridge::reset().
 */

#include "GateAudioProcessor.h"

#include "../util/TestUtils.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

class GateAudioProcessorResetTests : public juce::UnitTest
{
public:
    GateAudioProcessorResetTests() : UnitTest ("GateAudioProcessorResetTests") {}

    void runTest() override
    {
        beginTest ("Without reset, silence still flushes audio and the meters keep their values");
        testStaleStateSurvivesWithoutReset();

        beginTest ("reset() turns silent input into exact silence");
        testResetSilencesOutput();

        beginTest ("reset() clears the band meters (GateBandProcessor and BandLevelBridge)");
        testResetClearsBandMeters();

        beginTest ("reset() output is bit-identical to a fresh processor");
        testResetMatchesFreshProcessor();

        beginTest ("reset() clears the analyzer latency aligner");
        testResetClearsAnalyzerAligner();

        beginTest ("reset() is allocation-free");
        testResetIsAllocationFree();

        beginTest ("BandLevelBridge::reset() restores the floor level and keeps the band count");
        testBandLevelBridgeReset();
    }

private:
    static constexpr int kBlockSize = 512;
    static constexpr float kFloorDb = -80.0f;

    using Planar = std::array<std::vector<float>, 2>;

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

    static std::unique_ptr<GateAudioProcessor> createProcessor()
    {
        auto processor = std::make_unique<GateAudioProcessor>();
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);
        return processor;
    }

    static Planar process (GateAudioProcessor& processor, const Planar& input)
    {
        Planar output;
        const int totalSamples = static_cast<int> (input[0].size());

        for (int cursor = 0; cursor < totalSamples; cursor += kBlockSize)
        {
            const int blockSize = std::min (kBlockSize, totalSamples - cursor);
            juce::AudioBuffer<float> buffer (2, blockSize);
            for (int ch = 0; ch < 2; ++ch)
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

    static float maxAbs (const Planar& signal)
    {
        float peak = 0.0f;
        for (const auto& channel : signal)
            for (const float sample : channel)
                peak = std::max (peak, std::abs (sample));
        return peak;
    }

    /** Highest band level currently held by the processor's GateBandProcessor. */
    static float maxProcessorLevelDb (const GateAudioProcessor& processor)
    {
        std::array<float, GateBandProcessor::kMaxBands> levels {};
        const int numBands = processor.getObservedGateProcessorForTest()->getBandLevelsDb (levels.data());
        float peak = -1000.0f;
        for (int i = 0; i < numBands; ++i)
            peak = std::max (peak, levels[static_cast<std::size_t> (i)]);
        return peak;
    }

    /** Highest band level currently visible through the GUI bridge. */
    static float maxBridgeLevelDb (GateAudioProcessor& processor, int& numBandsOut)
    {
        std::array<float, BandLevelBridge::kMaxBands> levels {};
        numBandsOut = processor.getBandLevelBridge().read (levels.data());
        float peak = -1000.0f;
        for (int i = 0; i < numBandsOut; ++i)
            peak = std::max (peak, levels[static_cast<std::size_t> (i)]);
        return peak;
    }

    //==========================================================================

    void testStaleStateSurvivesWithoutReset()
    {
        auto processor = createProcessor();
        (void) process (*processor, makeNoise (kBlockSize * 16, 3U));

        int numBands = 0;
        expect (maxProcessorLevelDb (*processor) > kFloorDb, "Noise should raise the band meters");
        expect (maxBridgeLevelDb (*processor, numBands) > kFloorDb, "Noise should raise the bridge levels");

        const auto tail = process (*processor, makeSilence (kBlockSize * 2));
        expect (maxAbs (tail) > 0.0f,
                "Without reset(), old audio is still emitted when silence arrives");
    }

    void testResetSilencesOutput()
    {
        auto processor = createProcessor();
        const auto pre = process (*processor, makeNoise (kBlockSize * 16, 5U));
        expect (maxAbs (pre) > 0.0f, "Noise produced output before reset");

        processor->reset();

        const auto post = process (*processor, makeSilence (kBlockSize * 8));
        expect (maxAbs (post) <= 0.0f,
                "Silence after AudioProcessor::reset() must give exact zeros, peak = "
                + juce::String (maxAbs (post)));
    }

    void testResetClearsBandMeters()
    {
        auto processor = createProcessor();
        (void) process (*processor, makeNoise (kBlockSize * 16, 7U));

        int numBandsBefore = 0;
        expect (maxProcessorLevelDb (*processor) > kFloorDb, "Band meters are raised before reset");
        expect (maxBridgeLevelDb (*processor, numBandsBefore) > kFloorDb, "Bridge levels are raised before reset");
        expect (numBandsBefore > 0, "Bridge reports the active band count");

        processor->reset();

        expectEquals (maxProcessorLevelDb (*processor), kFloorDb,
                      "GateBandProcessor levels return to the floor after reset()");

        int numBandsAfter = 0;
        expectEquals (maxBridgeLevelDb (*processor, numBandsAfter), kFloorDb,
                      "BandLevelBridge levels return to the floor after reset()");
        expectEquals (numBandsAfter, numBandsBefore, "reset() keeps the reported band count");
    }

    void testResetMatchesFreshProcessor()
    {
        auto resetProcessor = createProcessor();
        (void) process (*resetProcessor, makeNoise (kBlockSize * 16 + 77, 11U));
        resetProcessor->reset();

        auto freshProcessor = createProcessor();

        const auto input = makeNoise (kBlockSize * 12, 13U);
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

        auto processor = createProcessor();
        (void) process (*processor, makeNoise (kSlot, 17U));

        processor->reset();
        (void) process (*processor, makeSilence (kSlot));

        std::array<float, kSlot> input {};
        std::array<float, kSlot> output {};
        const bool available = processor->getSpectrumBridge().tryConsume (input, output);
        expect (available, "A completed spectrum slot should be available");

        const float inputPeak = *std::max_element (input.begin(), input.end(),
                                                   [] (float a, float b) { return std::abs (a) < std::abs (b); });
        expect (std::abs (inputPeak) <= 0.0f,
                "Aligned analyzer input after reset() must not contain pre-reset audio, peak = "
                + juce::String (std::abs (inputPeak)));
    }

    void testResetIsAllocationFree()
    {
        auto processor = createProcessor();
        (void) process (*processor, makeNoise (kBlockSize * 4, 19U));

        TestUtils::AllocationCounter counter;
        counter.begin();
        processor->reset();
        counter.end();

        expect (counter.getCount() == 0,
                "AudioProcessor::reset() must not allocate - found "
                + juce::String (static_cast<int> (counter.getCount())));
    }

    void testBandLevelBridgeReset()
    {
        BandLevelBridge bridge;
        const float levels[5] = { -10.0f, -20.0f, -30.0f, -40.0f, -50.0f };
        bridge.update (levels, 5);

        bridge.reset();

        std::array<float, BandLevelBridge::kMaxBands> out {};
        const int numBands = bridge.read (out.data());
        expectEquals (numBands, 5, "reset() keeps the band count");
        for (int i = 0; i < numBands; ++i)
            expectEquals (out[static_cast<std::size_t> (i)], kFloorDb, "Band level returns to the floor");

        // The bridge keeps working after a reset.
        bridge.update (levels, 5);
        bridge.read (out.data());
        expectEquals (out[0], levels[0], "update() after reset() is visible");
    }
};

static GateAudioProcessorResetTests gateAudioProcessorResetTests;
