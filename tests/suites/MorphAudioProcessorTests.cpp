/**
 * @file MorphAudioProcessorTests.cpp
 * @brief JUCE UnitTest suite for MorphAudioProcessor.
 */

#include "MorphAudioProcessor.h"

#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>

#include <memory>

class MorphAudioProcessorTests : public juce::UnitTest
{
public:
    MorphAudioProcessorTests() : UnitTest ("MorphAudioProcessorTests") {}

    void runTest() override
    {
        beginTest ("Successful candidate apply uses depth-12 config");
        testSuccessfulCandidateApplyUsesDepth12Config();

        beginTest ("Rejected candidate rolls back fully");
        testRejectedCandidateRollsBackFully();

        beginTest ("Dirty-topology handoff does not pre-commit candidate state");
        testDirtyTopologyHandoffDoesNotPreCommitCandidateState();
    }

private:
    static std::unique_ptr<MorphAudioProcessor> createPreparedProcessor()
    {
        auto processor = std::make_unique<MorphAudioProcessor>();
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, 512);
        return processor;
    }

    void testSuccessfulCandidateApplyUsesDepth12Config()
    {
        auto processor = createPreparedProcessor();
        const auto fullDepth12 = TestUtils::getFullPacketDestinations (12);

        const bool applied = processor->applyTopologyCandidateForTest (fullDepth12);

        expect (applied, "A valid depth-12 candidate should be accepted");
        expectEquals (processor->getCommittedTopologyDepthForTest(), 12,
                      "Committed topology should reflect the accepted depth-12 candidate");
    }

    void testRejectedCandidateRollsBackFully()
    {
        auto processor = createPreparedProcessor();

        const auto committedDestinations = processor->getCommittedDestinationsForTest();
        const auto persistedTopology = processor->getPersistedTopologyStringForTest();
        const auto originalLatency = processor->getLatencySamples();
        const auto* originalMorphProcessor = processor->getObservedMorphProcessorForTest();
        const auto* originalEngine = processor->getEngineIdentityForTest();

        const std::vector<std::string> invalidCandidate { "LLLLLLLLLLLLL" };
        const bool applied = processor->applyTopologyCandidateForTest (invalidCandidate);

        expect (! applied, "An over-depth candidate should be rejected");
        expect (processor->getCommittedDestinationsForTest() == committedDestinations,
                "Rejected candidate must not replace committed destinations");
        expectEquals (processor->getPersistedTopologyStringForTest(), persistedTopology,
                      "Rejected candidate must not replace persisted topology");
        expectEquals (processor->getLatencySamples(), originalLatency,
                      "Rejected candidate must not change reported latency");
        expect (processor->getObservedMorphProcessorForTest() == originalMorphProcessor,
                "Rejected candidate must keep the observed MorphBandProcessor pointer");
        expect (processor->getEngineIdentityForTest() == originalEngine,
                "Rejected candidate must keep the active engine identity");
    }

    void testDirtyTopologyHandoffDoesNotPreCommitCandidateState()
    {
        auto processor = createPreparedProcessor();

        const auto originalPersistedTopology = processor->getPersistedTopologyStringForTest();
        const auto originalCommittedDestinations = processor->getCommittedDestinationsForTest();
        const std::vector<std::string> invalidCandidate { "LLLLLLLLLLLLL" };

        processor->getTopologyState().requestChange (invalidCandidate);
        const bool rebuilt = processor->rebuildPendingTopologyForTest();

        expect (! rebuilt, "Pending over-depth topology should be rejected");
        expectEquals (processor->getPersistedTopologyStringForTest(), originalPersistedTopology,
                      "Rejected pending topology must not overwrite persisted topology");
        expect (processor->getCommittedDestinationsForTest() == originalCommittedDestinations,
                "Rejected pending topology must not overwrite committed destinations");
    }
};

static MorphAudioProcessorTests morphAudioProcessorTests;
