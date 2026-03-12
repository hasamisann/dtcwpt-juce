#include "GateAudioProcessor.h"

#include "../../sample_plugins/common/InvalidTopologyDiagnostics.h"
#include "../../sample_plugins/common/TopologyPersistence.h"
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>

#include <memory>

class GateAudioProcessorTests : public juce::UnitTest
{
public:
    GateAudioProcessorTests() : juce::UnitTest ("GateAudioProcessorTests") {}

    void runTest() override
    {
        beginTest ("Valid candidate commits and updates persisted topology");
        testValidCandidateCommitsAndPersists();

        beginTest ("Invalid candidate preserves committed state and emits shared diagnostic");
        testInvalidCandidatePreservesCommittedStateAndEmitsDiagnostic();

        beginTest ("Dirty topology handoff does not pre-commit invalid candidate");
        testDirtyTopologyHandoffDoesNotPreCommitInvalidCandidate();

        beginTest ("Invalid persisted topology falls back to built-in default");
        testInvalidPersistedTopologyFallsBackToBuiltInDefault();

        beginTest ("Gate rejects valid core topology beyond depth-8 limit");
        testGateRejectsValidCoreTopologyBeyondDepthEightLimit();
    }

private:
    static constexpr int kBlockSize = 512;

    static const std::vector<std::string>& getDefaultTopology()
    {
        static const std::vector<std::string> kDefaultTopology
            { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" };
        return kDefaultTopology;
    }

    static std::unique_ptr<GateAudioProcessor> createPreparedProcessor()
    {
        auto processor = std::make_unique<GateAudioProcessor>();
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);
        return processor;
    }

    static juce::MemoryBlock createStateWithTopology (const juce::var& topologyProperty)
    {
        GateAudioProcessor processor;
        juce::ValueTree state = processor.getAPVTS().copyState();
        state.setProperty (topology::kTopologyPropertyKey, topologyProperty, nullptr);

        juce::MemoryBlock block;
        juce::MemoryOutputStream stream (block, true);
        state.writeToStream (stream);
        return block;
    }

    static juce::AudioBuffer<float> makeStereoBuffer (int numSamples)
    {
        juce::AudioBuffer<float> buffer (2, numSamples);
        buffer.clear();
        return buffer;
    }

    void testValidCandidateCommitsAndPersists()
    {
        auto processor = createPreparedProcessor();
        const auto validCandidate = TestUtils::getFullPacketDestinations (8);

        const bool applied = processor->applyTopologyCandidateForTest (validCandidate);

        expect (applied, "A valid depth-8 topology should be accepted by Gate");
        expect (processor->getCommittedDestinationsForTest() == validCandidate,
                "Accepted candidate should become the committed topology");
        expectEquals (processor->getPersistedTopologyStringForTest(),
                      topology::serializeTopologyDestinations (validCandidate),
                      "Accepted candidate should replace the persisted topology string");
    }

    void testInvalidCandidatePreservesCommittedStateAndEmitsDiagnostic()
    {
        auto processor = createPreparedProcessor();
        const auto validCandidate = TestUtils::getFullPacketDestinations (8);
        expect (processor->applyTopologyCandidateForTest (validCandidate));

        const auto committedDestinations = processor->getCommittedDestinationsForTest();
        const auto persistedTopology = processor->getPersistedTopologyStringForTest();
        const auto originalLatency = processor->getLatencySamples();
        const auto* originalGateProcessor = processor->getObservedGateProcessorForTest();
        const auto* originalEngine = processor->getEngineIdentityForTest();

        topology::resetInvalidTopologyDiagnosticsForTest();
        const std::vector<std::string> invalidCandidate { "L", "HL" };

        const bool applied = processor->applyTopologyCandidateForTest (invalidCandidate);

        expect (! applied, "Malformed topology should be rejected");
        expect (processor->getCommittedDestinationsForTest() == committedDestinations,
                "Rejected candidate must not replace committed destinations");
        expectEquals (processor->getPersistedTopologyStringForTest(), persistedTopology,
                      "Rejected candidate must not overwrite persisted topology");
        expectEquals (processor->getLatencySamples(), originalLatency,
                      "Rejected candidate must not change reported latency");
        expect (processor->getObservedGateProcessorForTest() == originalGateProcessor,
                "Rejected candidate must keep the observed GateBandProcessor pointer");
        expect (processor->getEngineIdentityForTest() == originalEngine,
                "Rejected candidate must keep the active engine identity");
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "Rejected candidate should emit the shared invalid-topology diagnostic");
        expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                      juce::String (topology::invalidTopologyDiagnosticCategory()),
                      "Rejected candidate should record the shared diagnostic category");
    }

    void testDirtyTopologyHandoffDoesNotPreCommitInvalidCandidate()
    {
        auto processor = createPreparedProcessor();
        const auto originalPersistedTopology = processor->getPersistedTopologyStringForTest();
        const auto originalCommittedDestinations = processor->getCommittedDestinationsForTest();
        const auto* originalEngine = processor->getEngineIdentityForTest();
        const auto* originalGateProcessor = processor->getObservedGateProcessorForTest();

        const std::vector<std::string> invalidCandidate { "L", "HL" };
        processor->getTopologyState().requestChange (invalidCandidate);

        auto buffer = makeStereoBuffer (64);
        juce::MidiBuffer midi;
        processor->processBlock (buffer, midi);

        expectEquals (processor->getPersistedTopologyStringForTest(), originalPersistedTopology,
                      "Rejected pending topology must not overwrite persisted topology");
        expect (processor->getCommittedDestinationsForTest() == originalCommittedDestinations,
                "Rejected pending topology must not overwrite committed destinations");
        expect (processor->getEngineIdentityForTest() == originalEngine,
                "Rejected pending topology must not replace the active engine");
        expect (processor->getObservedGateProcessorForTest() == originalGateProcessor,
                "Rejected pending topology must not replace the observed GateBandProcessor");
    }

    void testInvalidPersistedTopologyFallsBackToBuiltInDefault()
    {
        topology::resetInvalidTopologyDiagnosticsForTest();

        auto processor = std::make_unique<GateAudioProcessor>();
        const auto state = createStateWithTopology (juce::String { "L,HL" });
        processor->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);

        expect (processor->getCommittedDestinationsForTest() == getDefaultTopology(),
                "Invalid restored topology should fall back to Gate's built-in default topology");
        expectEquals (processor->getPersistedTopologyStringForTest(),
                      topology::serializeTopologyDestinations (getDefaultTopology()),
                      "Fallback path should commit the built-in default topology string");
        expect (processor->getEngineIdentityForTest() != nullptr,
                "Fallback path should leave Gate prepared with a valid engine");
        expect (processor->getObservedGateProcessorForTest() != nullptr,
                "Fallback path should leave Gate prepared with a valid GateBandProcessor");
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "Invalid restored topology should emit the shared invalid-topology diagnostic");
        expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                      juce::String (topology::invalidTopologyDiagnosticCategory()),
                      "Fallback path should use the shared diagnostic category");

        auto buffer = makeStereoBuffer (64);
        juce::MidiBuffer midi;
        processor->processBlock (buffer, midi);
        expect (processor->getEngineIdentityForTest() != nullptr,
                "Gate should remain usable after processing with the fallback topology");
    }

    void testGateRejectsValidCoreTopologyBeyondDepthEightLimit()
    {
        auto processor = createPreparedProcessor();
        const auto committedDestinations = processor->getCommittedDestinationsForTest();
        const auto persistedTopology = processor->getPersistedTopologyStringForTest();
        const auto validDepthNineCandidate = TestUtils::getFullPacketDestinations (9);

        topology::resetInvalidTopologyDiagnosticsForTest();
        const bool applied = processor->applyTopologyCandidateForTest (validDepthNineCandidate);

        expect (! applied, "Gate should reject topologies beyond its current depth-8 caller limit");
        expect (processor->getCommittedDestinationsForTest() == committedDestinations,
                "Depth-limit rejection must preserve the previously committed topology");
        expectEquals (processor->getPersistedTopologyStringForTest(), persistedTopology,
                      "Depth-limit rejection must preserve the persisted topology string");
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "Depth-limit rejection should emit the shared invalid-topology diagnostic");
        expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                      juce::String (topology::invalidTopologyDiagnosticCategory()),
                      "Depth-limit rejection should use the shared diagnostic category");
    }
};

static GateAudioProcessorTests gateAudioProcessorTests;
