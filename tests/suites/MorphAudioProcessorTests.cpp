/**
 * @file MorphAudioProcessorTests.cpp
 * @brief JUCE UnitTest suite for MorphAudioProcessor.
 */

#include "MorphAudioProcessor.h"

#include "../../sample_plugins/common/InvalidTopologyDiagnostics.h"
#include "../../sample_plugins/common/TopologyPersistence.h"
#include "../util/TestUtils.h"

#include <juce_audio_basics/juce_audio_basics.h>
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

        beginTest ("Invalid candidate preserves committed state and emits diagnostic");
        testInvalidCandidatePreservesCommittedStateAndEmitsDiagnostic();

        beginTest ("Dirty-topology handoff does not pre-commit candidate state");
        testDirtyTopologyHandoffDoesNotPreCommitCandidateState();

        beginTest ("Invalid persisted topology falls back to built-in default");
        testInvalidPersistedTopologyFallsBackToBuiltInDefault();
    }

private:
    static juce::MemoryBlock createStateWithTopologyProperty (const juce::var& topologyProperty)
    {
        juce::ValueTree state { "MorphParams" };
        state.setProperty (topology::kTopologyPropertyKey, topologyProperty, nullptr);

        juce::MemoryBlock stateData;
        juce::MemoryOutputStream stream (stateData, true);
        state.writeToStream (stream);
        return stateData;
    }

    static std::unique_ptr<MorphAudioProcessor> createPreparedProcessor()
    {
        auto processor = std::make_unique<MorphAudioProcessor>();
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, 512);
        return processor;
    }

    void testSuccessfulCandidateApplyUsesDepth12Config()
    {
        topology::resetInvalidTopologyDiagnosticsForTest();

        auto processor = createPreparedProcessor();
        const auto fullDepth12 = TestUtils::getFullPacketDestinations (12);
        const auto expectedPersistedTopology = topology::serializeTopologyDestinations (fullDepth12);

        const bool applied = processor->applyTopologyCandidateForTest (fullDepth12);

        expect (applied, "A valid depth-12 candidate should be accepted");
        expectEquals (processor->getCommittedTopologyDepthForTest(), 12,
                      "Committed topology should reflect the accepted depth-12 candidate");
        expect (processor->getPersistedTopologyStringForTest() == expectedPersistedTopology,
                "Accepted candidate must update persisted topology");
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 0,
                      "Valid candidate should not emit invalid-topology diagnostics");
    }

    void testInvalidCandidatePreservesCommittedStateAndEmitsDiagnostic()
    {
        auto processor = createPreparedProcessor();
        topology::resetInvalidTopologyDiagnosticsForTest();

        const auto validCandidate = TestUtils::getWaveletTreeDestinations (12);
        const bool validApplied = processor->applyTopologyCandidateForTest (validCandidate);
        expect (validApplied, "Setup should commit a valid topology before exercising rollback");

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
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "Rejected invalid candidate should emit one shared invalid-topology diagnostic");
        expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                      juce::String (topology::invalidTopologyDiagnosticCategory()),
                      "Rejected invalid candidate should emit the shared diagnostic category");
    }

    void testDirtyTopologyHandoffDoesNotPreCommitCandidateState()
    {
        topology::resetInvalidTopologyDiagnosticsForTest();

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
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "Rejected pending topology should emit one shared invalid-topology diagnostic");
    }

    void testInvalidPersistedTopologyFallsBackToBuiltInDefault()
    {
        topology::resetInvalidTopologyDiagnosticsForTest();

        auto processor = std::make_unique<MorphAudioProcessor>();
        const juce::MemoryBlock invalidState = createStateWithTopologyProperty ("LLLLLLLLLLLLL");

        processor->setStateInformation (invalidState.getData(), static_cast<int> (invalidState.getSize()));
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, 512);

        juce::AudioBuffer<float> buffer (2, 64);
        buffer.clear();
        juce::MidiBuffer midi;
        processor->processBlock (buffer, midi);

        const auto expectedDefaultTopology = topology::serializeTopologyDestinations (
            std::vector<std::string> { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" });

        expect (processor->getEngineIdentityForTest() != nullptr,
                "Processor should keep a valid engine after invalid restore fallback");
        expect (processor->getObservedMorphProcessorForTest() != nullptr,
                "Processor should keep a valid band processor after invalid restore fallback");
        expect (processor->getCommittedDestinationsForTest()
                == std::vector<std::string> { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" },
                "Invalid persisted topology should fall back to the built-in default topology");
        expect (processor->getPersistedTopologyStringForTest() == expectedDefaultTopology,
                "Fallback should store the built-in default topology back into APVTS state");
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "Invalid persisted topology should emit one shared invalid-topology diagnostic");
        expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                      juce::String (topology::invalidTopologyDiagnosticCategory()),
                      "Invalid persisted topology should emit the shared diagnostic category");
    }
};

static MorphAudioProcessorTests morphAudioProcessorTests;
