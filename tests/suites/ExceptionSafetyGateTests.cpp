/**
 * @file ExceptionSafetyGateTests.cpp
 * @brief Exception safety of the Gate plugin's topology and engine construction (issue #7).
 *
 * Two suites live here:
 *  - GateExceptionSafetyInjectionTests injects std::bad_alloc / std::runtime_error / std::system_error
 *    into noexcept functions. Before the fix these terminate the whole test process, so the suite
 *    is kept separate and can be run on its own with `--test GateExceptionSafetyInjectionTests`.
 *  - GateExceptionSafetyTests covers the behaviour when no engine could be constructed.
 */

#include "GateAudioProcessor.h"

#include "../../sample_plugins/common/InvalidTopologyDiagnostics.h"
#include "../../sample_plugins/common/TopologyPersistence.h"
#include "../util/ExceptionSafetyTestUtils.h"
#include "../util/TestUtils.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <memory>
#include <string>
#include <vector>

namespace
{
using ExceptionSafetyTestUtils::isAllFinite;
using ExceptionSafetyTestUtils::isAllZero;
using ExceptionSafetyTestUtils::makeConstantBuffer;
using ExceptionSafetyTestUtils::makeToneBuffer;

constexpr int kBlockSize = 512;
using Failpoint = dtcwpt::DTCWPTProcessor::PrepareCandidateFailpoint;

std::unique_ptr<GateAudioProcessor> createPreparedProcessor()
{
    auto processor = std::make_unique<GateAudioProcessor>();
    processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);
    return processor;
}

struct CommittedSnapshot
{
    const dtcwpt::DTCWPTProcessor* engine = nullptr;
    const GateBandProcessor* bandProcessor = nullptr;
    std::vector<std::string> destinations;
    juce::String persistedTopology;
    int latency = 0;
    int analyzerDelay = 0;
};

CommittedSnapshot takeSnapshot (GateAudioProcessor& processor)
{
    CommittedSnapshot snapshot;
    snapshot.engine = processor.getEngineIdentityForTest();
    snapshot.bandProcessor = processor.getObservedGateProcessorForTest();
    snapshot.destinations = processor.getCommittedDestinationsForTest();
    snapshot.persistedTopology = processor.getPersistedTopologyStringForTest();
    snapshot.latency = processor.getLatencySamples();
    snapshot.analyzerDelay = processor.getAnalyzerInputDelaySamplesForTest();
    return snapshot;
}

juce::MemoryBlock createStateWithCustomTopology (const std::vector<std::string>& destinations)
{
    GateAudioProcessor source;
    source.prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);
    const bool applied = source.applyTopologyCandidateForTest (destinations);
    jassert (applied);
    juce::ignoreUnused (applied);

    juce::MemoryBlock state;
    source.getStateInformation (state);
    return state;
}
} // namespace

//==============================================================================
class GateExceptionSafetyInjectionTests : public juce::UnitTest
{
public:
    GateExceptionSafetyInjectionTests() : UnitTest ("GateExceptionSafetyInjectionTests") {}

    void runTest() override
    {
        beginTest ("std::bad_alloc while building a candidate engine keeps the committed state");
        testCandidateFailureKeepsCommittedState (Failpoint::beforeCommitBadAlloc);

        beginTest ("std::runtime_error while building a candidate engine keeps the committed state");
        testCandidateFailureKeepsCommittedState (Failpoint::beforeCommit);

        beginTest ("Only std::invalid_argument emits the invalid-topology diagnostic");
        testOnlyInvalidArgumentEmitsDiagnostic();

        beginTest ("A failed candidate build does not block a later valid topology");
        testLaterValidTopologyStillApplies();

        beginTest ("Initial build falls back to the default topology when the candidate build fails");
        testInitialBuildFallsBackToDefaultTopology();



    }

private:
    static std::vector<std::string> committedTopology() { return TestUtils::getWaveletTreeDestinations (5); }
    static std::vector<std::string> defaultTopology()
    {
        return { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" };
    }
    static std::vector<std::string> candidateTopology() { return TestUtils::getWaveletTreeDestinations (7); }

    void testCandidateFailureKeepsCommittedState (Failpoint failpoint)
    {
        auto processor = createPreparedProcessor();
        expect (processor->applyTopologyCandidateForTest (committedTopology()), "Committed topology should apply");

        const auto before = takeSnapshot (*processor);

        processor->failNextEngineBuildsForTest (failpoint, 1);
        const bool applied = processor->applyTopologyCandidateForTest (candidateTopology());

        expect (! applied, "A failed engine construction must be reported as not applied");

        const auto after = takeSnapshot (*processor);
        expect (after.engine == before.engine, "Engine instance must be kept");
        expect (after.bandProcessor == before.bandProcessor, "Band processor instance must be kept");
        expect (after.destinations == before.destinations, "Committed destinations must be kept");
        expectEquals (after.persistedTopology, before.persistedTopology, "Persisted topology property must be kept");
        expectEquals (after.latency, before.latency, "Reported latency must be kept");
        expectEquals (after.analyzerDelay, before.analyzerDelay, "Analyzer alignment delay must be kept");
        expect (processor->isEngineAvailable(), "The previous engine is still usable");

        auto block = makeToneBuffer (kBlockSize, 0);
        juce::MidiBuffer midi;
        processor->processBlock (block, midi);
        expect (isAllFinite (block), "The kept engine must still process audio");
    }

    void testOnlyInvalidArgumentEmitsDiagnostic()
    {
        auto processor = createPreparedProcessor();

        topology::resetInvalidTopologyDiagnosticsForTest();
        processor->failNextEngineBuildsForTest (Failpoint::beforeCommitBadAlloc, 1);
        expect (! processor->applyTopologyCandidateForTest (candidateTopology()));
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 0,
                      "A non-topology failure must not be reported as an invalid topology");

#if ! JUCE_DEBUG
        // emitInvalidTopologyDiagnostic() asserts with jassertfalse, which traps in Debug builds.
        expect (! processor->applyTopologyCandidateForTest ({ "LLLLLLLLLLLLL" }));
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "std::invalid_argument keeps emitting exactly one diagnostic");
#endif
    }

    void testLaterValidTopologyStillApplies()
    {
        auto processor = createPreparedProcessor();

        processor->failNextEngineBuildsForTest (Failpoint::beforeCommitBadAlloc, 1);
        expect (! processor->applyTopologyCandidateForTest (candidateTopology()));
        expect (processor->applyTopologyCandidateForTest (candidateTopology()),
                "The next attempt succeeds once the failure is gone");
        expect (processor->getCommittedDestinationsForTest() == candidateTopology());
    }

    void testInitialBuildFallsBackToDefaultTopology()
    {
        const auto state = createStateWithCustomTopology (candidateTopology());

        GateAudioProcessor processor;
        processor.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        processor.failNextEngineBuildsForTest (Failpoint::beforeCommitBadAlloc, 1);
        processor.prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);

        expect (processor.isEngineAvailable(), "The default topology provides a usable engine");
        expect (processor.getEngineIdentityForTest() != nullptr);
        expect (processor.getCommittedDestinationsForTest() == defaultTopology(),
                "The committed topology is the 6-level default");
    }






};

//==============================================================================
class GateExceptionSafetyTests : public juce::UnitTest
{
public:
    GateExceptionSafetyTests() : UnitTest ("GateExceptionSafetyTests") {}

    void runTest() override
    {
        beginTest ("processBlock is silent and the failure is exposed when no engine could be built");
        testNoEngineIsSilentAndReported();

        beginTest ("Engine availability recovers on the next successful prepareToPlay");
        testAvailabilityRecovers();

        beginTest ("A healthy processor reports an available engine");
        testHealthyProcessorIsAvailable();
    }

private:
    /** A non-positive sample rate makes both the candidate and the default topology fail with std::invalid_argument. */
    static std::unique_ptr<GateAudioProcessor> createProcessorWithoutEngine()
    {
        topology::resetInvalidTopologyDiagnosticsForTest();
        auto processor = std::make_unique<GateAudioProcessor>();
        processor->prepareToPlay (0.0, kBlockSize);
        return processor;
    }

    void testNoEngineIsSilentAndReported()
    {
        auto processor = createProcessorWithoutEngine();

        expect (processor->getEngineIdentityForTest() == nullptr, "No engine could be built");
        expect (! processor->isEngineAvailable(), "The failure must be visible through isEngineAvailable()");
        expectEquals (processor->getLatencySamples(), 0);

        auto block = makeConstantBuffer (kBlockSize, 0.5f);
        juce::MidiBuffer midi;
        processor->processBlock (block, midi);

        expect (isAllZero (block), "Output must be muted instead of passing the input through");
    }

    void testAvailabilityRecovers()
    {
        auto processor = createProcessorWithoutEngine();
        expect (! processor->isEngineAvailable());

        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);

        expect (processor->isEngineAvailable(), "A successful prepareToPlay restores availability");
        expect (processor->getEngineIdentityForTest() != nullptr);
    }

    void testHealthyProcessorIsAvailable()
    {
        auto processor = createPreparedProcessor();
        expect (processor->isEngineAvailable());
    }
};

static GateExceptionSafetyInjectionTests exceptionSafetyGateInjectionTests;
static GateExceptionSafetyTests exceptionSafetyGateTests;
