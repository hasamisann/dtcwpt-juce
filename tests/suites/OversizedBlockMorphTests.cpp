/**
 * @file OversizedBlockMorphTests.cpp
 * @brief Morph regression tests for hosts that exceed the prepareToPlay block size.
 *
 * Output for oversized or irregular host blocks must match the output for blocks that
 * respect the prepared maximum, and processBlock must not allocate on the audio thread.
 */

#include "MorphAudioProcessor.h"

#include "../util/OversizedBlockHelpers.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <memory>

class OversizedBlockMorphTests : public juce::UnitTest
{
public:
    OversizedBlockMorphTests() : juce::UnitTest ("OversizedBlockMorphTests") {}

    void runTest() override
    {
        beginTest ("Output matches the prepared-size run for oversized and irregular blocks");
        testMatchesPreparedSizeRun (2, "Morph");

        beginTest ("Output with a sidechain bus matches the prepared-size run for oversized blocks");
        testMatchesPreparedSizeRun (4, "Morph (sidechain)");

        beginTest ("processBlock does not allocate for oversized blocks");
        testOversizedBlocksDoNotAllocate();
    }

private:
    static std::unique_ptr<MorphAudioProcessor> createPreparedProcessor()
    {
        auto processor = std::make_unique<MorphAudioProcessor>();
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, OversizedBlockHelpers::kPreparedBlockSize);
        return processor;
    }

    void testMatchesPreparedSizeRun (int numChannels, const juce::String& label)
    {
        using namespace OversizedBlockHelpers;
        const auto signal = makeSignal (numChannels, kTotalSamples);

        auto referenceProcessor = createPreparedProcessor();
        const auto reference = run (*referenceProcessor, signal, { kPreparedBlockSize });

        for (const auto& pattern : getHostilePatterns())
        {
            auto processor = createPreparedProcessor();
            expect (run (*processor, signal, pattern) == reference,
                    label + " output for host blocks starting with " + juce::String (pattern.front())
                        + " (pattern of " + juce::String (static_cast<int> (pattern.size())) + ")"
                        + " should be bit-identical to the " + juce::String (kPreparedBlockSize)
                        + "-sample run");
        }
    }

    void testOversizedBlocksDoNotAllocate()
    {
        auto processor = createPreparedProcessor();
        juce::AudioBuffer<float> buffer (4, 1024);
        buffer.clear();
        juce::MidiBuffer midi;

        TestUtils::AllocationCounter counter;
        counter.begin();
        for (int iteration = 0; iteration < 4; ++iteration)
            processor->processBlock (buffer, midi);
        counter.end();

        expect (counter.getCount() == 0,
                "Oversized Morph processBlock calls should not allocate; found "
                    + juce::String (static_cast<int> (counter.getCount())) + " allocations");
    }
};

static OversizedBlockMorphTests oversizedBlockMorphTests;
