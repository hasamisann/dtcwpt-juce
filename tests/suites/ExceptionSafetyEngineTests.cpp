/**
 * @file ExceptionSafetyEngineTests.cpp
 * @brief Audio-path exception safety of DTCWPTProcessor (issue #7, problem 2).
 *
 * The audio-thread entry points must never throw: they are declared noexcept, and the
 * internal-inconsistency checks in the analysis stage degrade to dropping the affected
 * sample instead of raising std::runtime_error.
 */

#include <dtcwpt/dtcwpt_band_processor.h>
#include <dtcwpt/dtcwpt_processor.h>

#include "../util/ExceptionSafetyTestUtils.h"
#include "../util/TestUtils.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <exception>
#include <memory>
#include <utility>

class ExceptionSafetyEngineTests : public juce::UnitTest
{
public:
    ExceptionSafetyEngineTests() : UnitTest ("ExceptionSafetyEngineTests") {}

    void runTest() override
    {
        beginTest ("processBlock and processSidechain are declared noexcept");
        testAudioEntryPointsAreNoexcept();

#if DTCWPT_ENABLE_TEST_SEAMS && ! JUCE_DEBUG
        // The fallbacks assert with jassertfalse, which traps in Debug builds.
        beginTest ("Analysis node lookup failure does not throw out of processBlock");
        testAnalysisFaultDoesNotThrow (dtcwpt::DTCWPTProcessor::AnalysisFaultForTesting::internalNodeLookup);

        beginTest ("Analysis cursor overflow does not throw out of processBlock");
        testAnalysisFaultDoesNotThrow (dtcwpt::DTCWPTProcessor::AnalysisFaultForTesting::cursorOverflow);
#endif
    }

private:
    class PassthroughProcessor : public dtcwpt::BandProcessor
    {
    public:
        void prepare (double, int, int, int) override {}
        void reset() override {}
    };

    void testAudioEntryPointsAreNoexcept()
    {
        dtcwpt::DTCWPTProcessor processor;
        juce::AudioBuffer<double> buffer;

        expect (noexcept (processor.processBlock (buffer)),
                "DTCWPTProcessor::processBlock must be noexcept");
        expect (noexcept (processor.processSidechain (buffer)),
                "DTCWPTProcessor::processSidechain must be noexcept");
    }

#if DTCWPT_ENABLE_TEST_SEAMS
    void testAnalysisFaultDoesNotThrow (dtcwpt::DTCWPTProcessor::AnalysisFaultForTesting fault)
    {
        constexpr int kBlock = 512;

        dtcwpt::TopologyConfig config;
        config.destinations = TestUtils::getFullPacketDestinations (4);
        config.maxDepth = 8;

        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor (std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlock, config, 2);
        processor.setAnalysisFaultForTesting (fault);

        const auto noise = TestUtils::generateNoise (0.25, TestUtils::TestConfig::SAMPLE_RATE, 7U);

        bool threw = false;
        bool finite = true;

        try
        {
            for (int block = 0; block < 6; ++block)
            {
                juce::AudioBuffer<double> main (2, kBlock);
                juce::AudioBuffer<double> sidechain (2, kBlock);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < kBlock; ++i)
                    {
                        const auto sample = noise[static_cast<size_t> ((block * kBlock + i) % static_cast<int> (noise.size()))];
                        main.setSample (ch, i, sample);
                        sidechain.setSample (ch, i, sample);
                    }

                processor.processSidechain (sidechain);
                processor.processBlock (main);
                finite = finite && ExceptionSafetyTestUtils::isAllFinite (main);
            }
        }
        catch (const std::exception& e)
        {
            threw = true;
            logMessage (juce::String ("exception escaped processBlock: ") + e.what());
        }

        expect (! threw, "processBlock must not throw on internal inconsistency");
        expect (finite, "Output must stay finite when a sample is dropped");

        // The engine recovers once the inconsistency is gone.
        processor.setAnalysisFaultForTesting (dtcwpt::DTCWPTProcessor::AnalysisFaultForTesting::none);
        juce::AudioBuffer<double> recovered (2, kBlock);
        recovered.clear();
        processor.processBlock (recovered);
        expect (ExceptionSafetyTestUtils::isAllFinite (recovered), "Engine keeps running after the fault clears");
    }
#endif
};

static ExceptionSafetyEngineTests exceptionSafetyEngineTests;
