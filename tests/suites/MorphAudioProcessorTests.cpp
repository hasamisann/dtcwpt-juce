/**
 * @file MorphAudioProcessorTests.cpp
 * @brief JUCE UnitTest suite for MorphAudioProcessor.
 */

#include "MorphAudioProcessor.h"

#include "../../sample_plugins/common/InvalidTopologyDiagnostics.h"
#include "../../sample_plugins/common/TopologyPersistence.h"
#include "../util/RegressionFixtures.h"
#include "../util/TestUtils.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

class MorphAudioProcessorTests : public juce::UnitTest
{
public:
    MorphAudioProcessorTests() : UnitTest ("MorphAudioProcessorTests") {}

    void runTest() override
    {
        beginTest ("Fresh processors produce deterministic output and committed state");
        testFreshProcessorsProduceDeterministicOutputAndCommittedState();

        beginTest ("No-sidechain fallback matches explicit duplicated sidechain");
        testNoSidechainFallbackMatchesExplicitDuplicatedSidechain();

        beginTest ("Invalid hot-swap preserves committed topology and observable output");
        testInvalidHotSwapPreservesCommittedTopologyAndObservableOutput();

        beginTest ("Invalid persisted topology falls back to built-in default");
        testInvalidPersistedTopologyFallsBackToBuiltInDefault();

        beginTest ("Valid persisted state round-trips topology and processor behavior");
        testValidPersistedStateRoundTripsTopologyAndProcessorBehavior();

        beginTest ("Bridge snapshots stay latency-aligned after prepare rebuild and restore");
        testBridgeSnapshotsStayLatencyAlignedAfterPrepareRebuildAndRestore();

        beginTest ("Committed processBlock scenarios remain allocation-free");
        testCommittedProcessBlockScenariosRemainAllocationFree();

        beginTest ("Mixed-depth committed Morph topology remains deterministic under the worklist scheduler");
        testMixedDepthCommittedMorphTopologyRemainsDeterministicUnderTheWorklistScheduler();
    }

private:
    static constexpr int kBlockSize = 512;

    struct MorphParameterState
    {
        float magnitude = 0.0f;
        float phase = 0.7f;
        float thresholdDb = -20.0f;
        bool bypassLow = true;
        bool bypassHigh = false;
    };

    struct StereoSignal
    {
        std::vector<float> left;
        std::vector<float> right;
    };

    struct ProcessRunResult
    {
        RegressionFixtures::QuantizedBufferView output;
        std::vector<std::string> committedDestinations;
        juce::String persistedTopology;
        int latencySamples = 0;
        int analyzerDelaySamples = 0;
    };

    struct SpectrumSnapshot
    {
        bool available = false;
        std::array<float, SpectrumDataBridge::kSlotSize> input {};
        std::array<float, SpectrumDataBridge::kSlotSize> output {};
    };

    static const std::vector<std::string>& getDefaultTopology()
    {
        static const std::vector<std::string> kDefaultTopology
            { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" };
        return kDefaultTopology;
    }

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
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);
        return processor;
    }

    static void setFloatParameter (MorphAudioProcessor& processor, const juce::String& parameterId, float value)
    {
        auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (processor.getAPVTS().getParameter (parameterId));
        jassert (parameter != nullptr);
        if (parameter != nullptr)
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    static void setBoolParameter (MorphAudioProcessor& processor, const juce::String& parameterId, bool value)
    {
        if (auto* parameter = processor.getAPVTS().getParameter (parameterId))
            parameter->setValueNotifyingHost (value ? 1.0f : 0.0f);
    }

    static void applyParameterState (MorphAudioProcessor& processor, const MorphParameterState& parameterState)
    {
        setFloatParameter (processor, ParamID::Magnitude, parameterState.magnitude);
        setFloatParameter (processor, ParamID::Phase, parameterState.phase);
        setFloatParameter (processor, ParamID::Threshold, parameterState.thresholdDb);
        setBoolParameter (processor, ParamID::BypassLow, parameterState.bypassLow);
        setBoolParameter (processor, ParamID::BypassHigh, parameterState.bypassHigh);
    }

    static StereoSignal makeSignal (int numSamples,
                                    double leftPrimaryHz,
                                    double leftSecondaryHz,
                                    double rightPrimaryHz,
                                    double rightSecondaryHz)
    {
        StereoSignal signal;
        signal.left.resize (static_cast<std::size_t> (numSamples));
        signal.right.resize (static_cast<std::size_t> (numSamples));

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const double timeSeconds = static_cast<double> (sample) / TestUtils::TestConfig::SAMPLE_RATE;
            const double left = 0.42 * std::sin (juce::MathConstants<double>::twoPi * leftPrimaryHz * timeSeconds)
                              + 0.11 * std::cos (juce::MathConstants<double>::twoPi * leftSecondaryHz * timeSeconds);
            const double right = 0.37 * std::cos (juce::MathConstants<double>::twoPi * rightPrimaryHz * timeSeconds)
                               + 0.09 * std::sin (juce::MathConstants<double>::twoPi * rightSecondaryHz * timeSeconds);

            signal.left[static_cast<std::size_t> (sample)] = static_cast<float> (left);
            signal.right[static_cast<std::size_t> (sample)] = static_cast<float> (right);
        }

        return signal;
    }

    static juce::AudioBuffer<float> makeProcessBuffer (const StereoSignal& mainSignal,
                                                       const std::optional<StereoSignal>& sidechainSignal)
    {
        const int numSamples = static_cast<int> (mainSignal.left.size());
        const int numChannels = sidechainSignal.has_value() ? 4 : 2;
        juce::AudioBuffer<float> buffer (numChannels, numSamples);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto sampleIndex = static_cast<std::size_t> (sample);
            buffer.setSample (0, sample, mainSignal.left[sampleIndex]);
            buffer.setSample (1, sample, mainSignal.right[sampleIndex]);
        }

        if (sidechainSignal.has_value())
        {
            jassert (sidechainSignal->left.size() == mainSignal.left.size());
            jassert (sidechainSignal->right.size() == mainSignal.right.size());

            for (int sample = 0; sample < numSamples; ++sample)
            {
                const auto sampleIndex = static_cast<std::size_t> (sample);
                buffer.setSample (2, sample, sidechainSignal->left[sampleIndex]);
                buffer.setSample (3, sample, sidechainSignal->right[sampleIndex]);
            }
        }

        return buffer;
    }

    static RegressionFixtures::QuantizedBufferView captureQuantizedMainOutput (const juce::AudioBuffer<float>& buffer)
    {
        std::vector<double> samples;
        const int numSamples = buffer.getNumSamples();
        samples.reserve (static_cast<std::size_t> (2 * numSamples));

        for (int sample = 0; sample < numSamples; ++sample)
        {
            for (int channel = 0; channel < 2; ++channel)
                samples.push_back (static_cast<double> (buffer.getSample (channel, sample)));
        }

        return RegressionFixtures::quantizeToFloat32 (samples, 2, numSamples);
    }

    static ProcessRunResult processOnce (MorphAudioProcessor& processor,
                                         const StereoSignal& mainSignal,
                                         const std::optional<StereoSignal>& sidechainSignal)
    {
        auto buffer = makeProcessBuffer (mainSignal, sidechainSignal);
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);

        ProcessRunResult result;
        result.output = captureQuantizedMainOutput (buffer);
        result.committedDestinations = processor.getCommittedDestinationsForTest();
        result.persistedTopology = processor.getPersistedTopologyStringForTest();
        result.latencySamples = processor.getLatencySamples();
        result.analyzerDelaySamples = processor.getAnalyzerInputDelaySamplesForTest();
        return result;
    }

    static juce::MemoryBlock getStateInformationCopy (MorphAudioProcessor& processor)
    {
        juce::MemoryBlock state;
        processor.getStateInformation (state);
        return state;
    }

    static int findPeakIndex (const std::array<float, SpectrumDataBridge::kSlotSize>& samples) noexcept
    {
        int peakIndex = 0;
        float peakMagnitude = 0.0f;

        for (int index = 0; index < SpectrumDataBridge::kSlotSize; ++index)
        {
            const float magnitude = std::abs (samples[static_cast<std::size_t> (index)]);
            if (magnitude > peakMagnitude)
            {
                peakMagnitude = magnitude;
                peakIndex = index;
            }
        }

        return peakIndex;
    }

    static SpectrumSnapshot captureImpulseBridgeSnapshot (MorphAudioProcessor& processor)
    {
        constexpr int kBlocksPerSnapshot = SpectrumDataBridge::kSlotSize / kBlockSize;
        static_assert (SpectrumDataBridge::kSlotSize % kBlockSize == 0,
                       "Bridge snapshot helper expects the slot size to be divisible by the test block size");

        for (int block = 0; block < kBlocksPerSnapshot; ++block)
        {
            juce::AudioBuffer<float> buffer (2, kBlockSize);
            buffer.clear();

            if (block == 0)
            {
                buffer.setSample (0, 0, 1.0f);
                buffer.setSample (1, 0, 1.0f);
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }

        SpectrumSnapshot snapshot;
        snapshot.available = processor.consumeSpectrumSnapshotForTest (snapshot.input, snapshot.output);
        return snapshot;
    }

    void expectQuantizedOutputsEqual (const RegressionFixtures::QuantizedBufferView& expected,
                                      const RegressionFixtures::QuantizedBufferView& actual,
                                      const juce::String& context)
    {
        juce::String failureMessage;
        expect (RegressionFixtures::compareQuantizedBuffers (expected, actual, failureMessage),
                context + ": " + failureMessage);
    }

    void expectLatencyAlignedSnapshot (MorphAudioProcessor& processor, const juce::String& context)
    {
        const int latencySamples = processor.getLatencySamples();
        expect (latencySamples < SpectrumDataBridge::kSlotSize,
                context + ": latency must fit within one bridge snapshot");
        expectEquals (processor.getAnalyzerInputDelaySamplesForTest(), latencySamples,
                      context + ": analyzer delay should match committed processor latency");

        const auto snapshot = captureImpulseBridgeSnapshot (processor);
        expect (snapshot.available, context + ": expected a consumable bridge snapshot after four blocks");

        if (! snapshot.available)
            return;

        const int inputPeakIndex = findPeakIndex (snapshot.input);
        const int outputPeakIndex = findPeakIndex (snapshot.output);
        expectEquals (inputPeakIndex, latencySamples,
                      context + ": aligned input bridge peak should land at the committed latency");
        expectEquals (outputPeakIndex, latencySamples,
                      context + ": output bridge peak should land at the committed latency");
        expectEquals (inputPeakIndex, outputPeakIndex,
                      context + ": aligned input and output peaks should match");
    }

    void testFreshProcessorsProduceDeterministicOutputAndCommittedState()
    {
        const auto topologyCandidate = TestUtils::getFullPacketDestinations (12);
        const auto expectedPersistedTopology = topology::serializeTopologyDestinations (topologyCandidate);
        const MorphParameterState parameterState { 0.63f, 0.28f, -48.0f, false, true };
        const auto mainSignal = makeSignal (kBlockSize, 311.0, 1247.0, 523.25, 1760.0);
        const auto sidechainSignal = makeSignal (kBlockSize, 197.0, 880.0, 659.25, 987.77);

        auto firstProcessor = createPreparedProcessor();
        auto secondProcessor = createPreparedProcessor();

        expect (firstProcessor->applyTopologyCandidateForTest (topologyCandidate),
                "Reference processor should accept the valid depth-12 topology");
        expect (secondProcessor->applyTopologyCandidateForTest (topologyCandidate),
                "Determinism comparison processor should accept the same valid topology");

        applyParameterState (*firstProcessor, parameterState);
        applyParameterState (*secondProcessor, parameterState);

        const auto firstRun = processOnce (*firstProcessor, mainSignal, sidechainSignal);
        const auto secondRun = processOnce (*secondProcessor, mainSignal, sidechainSignal);

        expectQuantizedOutputsEqual (firstRun.output, secondRun.output,
                                     "Fresh processors with matching state should produce identical quantized audio");
        expect (firstRun.committedDestinations == topologyCandidate,
                "Reference processor should keep the committed topology after processing");
        expect (secondRun.committedDestinations == topologyCandidate,
                "Comparison processor should keep the committed topology after processing");
        expectEquals (firstRun.persistedTopology, expectedPersistedTopology,
                      "Reference processor should persist the committed topology string");
        expectEquals (secondRun.persistedTopology, expectedPersistedTopology,
                      "Comparison processor should persist the committed topology string");
        expectEquals (firstRun.latencySamples, secondRun.latencySamples,
                      "Fresh processors should report the same committed latency");
        expectEquals (firstRun.analyzerDelaySamples, secondRun.analyzerDelaySamples,
                      "Fresh processors should expose the same analyzer delay");
    }

    void testNoSidechainFallbackMatchesExplicitDuplicatedSidechain()
    {
        const auto topologyCandidate = TestUtils::getWaveletTreeDestinations (12);
        const auto mainSignal = makeSignal (kBlockSize, 261.63, 740.0, 392.0, 1318.51);
        const MorphParameterState parameterState { 0.82f, 0.61f, -72.0f, false, false };

        auto noSidechainProcessor = createPreparedProcessor();
        auto duplicatedSidechainProcessor = createPreparedProcessor();

        expect (noSidechainProcessor->applyTopologyCandidateForTest (topologyCandidate),
                "No-sidechain processor should accept the committed topology");
        expect (duplicatedSidechainProcessor->applyTopologyCandidateForTest (topologyCandidate),
                "Duplicated-sidechain processor should accept the committed topology");

        applyParameterState (*noSidechainProcessor, parameterState);
        applyParameterState (*duplicatedSidechainProcessor, parameterState);

        const auto fallbackRun = processOnce (*noSidechainProcessor, mainSignal, std::nullopt);
        const auto duplicatedRun = processOnce (*duplicatedSidechainProcessor, mainSignal, mainSignal);

        expectQuantizedOutputsEqual (fallbackRun.output, duplicatedRun.output,
                                     "Missing sidechain input should behave like explicitly duplicating the main input");
        expect (fallbackRun.committedDestinations == duplicatedRun.committedDestinations,
                "Fallback and duplicated-sidechain runs should keep the same committed topology");
        expectEquals (fallbackRun.persistedTopology, duplicatedRun.persistedTopology,
                      "Fallback and duplicated-sidechain runs should keep the same persisted topology string");
        expectEquals (fallbackRun.latencySamples, duplicatedRun.latencySamples,
                      "Fallback and duplicated-sidechain runs should report the same latency");
    }

    void testInvalidHotSwapPreservesCommittedTopologyAndObservableOutput()
    {
        const auto committedTopology = TestUtils::getWaveletTreeDestinations (12);
        const auto expectedPersistedTopology = topology::serializeTopologyDestinations (committedTopology);
        const MorphParameterState parameterState { 0.57f, 0.39f, -54.0f, false, false };
        const auto mainSignal = makeSignal (kBlockSize, 430.0, 1290.0, 860.0, 1720.0);
        const auto sidechainSignal = makeSignal (kBlockSize, 210.0, 640.0, 510.0, 1530.0);

        auto baselineProcessor = createPreparedProcessor();
        auto guardedProcessor = createPreparedProcessor();

        expect (baselineProcessor->applyTopologyCandidateForTest (committedTopology),
                "Baseline processor should accept the committed topology");
        expect (guardedProcessor->applyTopologyCandidateForTest (committedTopology),
                "Guarded processor should accept the committed topology before the invalid hot-swap attempt");

        applyParameterState (*baselineProcessor, parameterState);
        applyParameterState (*guardedProcessor, parameterState);

        const auto* originalEngine = guardedProcessor->getEngineIdentityForTest();
        const auto* originalMorphProcessor = guardedProcessor->getObservedMorphProcessorForTest();

        topology::resetInvalidTopologyDiagnosticsForTest();
        guardedProcessor->getTopologyState().requestChange ({ "LLLLLLLLLLLLL" });

        const auto baselineRun = processOnce (*baselineProcessor, mainSignal, sidechainSignal);
        const auto guardedRun = processOnce (*guardedProcessor, mainSignal, sidechainSignal);

        expectQuantizedOutputsEqual (baselineRun.output, guardedRun.output,
                                     "Rejected hot-swap should leave observable audio identical to the committed reference run");
        expect (guardedRun.committedDestinations == committedTopology,
                "Rejected hot-swap should preserve the committed topology");
        expectEquals (guardedRun.persistedTopology, expectedPersistedTopology,
                      "Rejected hot-swap should preserve the persisted topology string");
        expectEquals (guardedRun.latencySamples, baselineRun.latencySamples,
                      "Rejected hot-swap should preserve the committed latency");
        expect (guardedProcessor->getEngineIdentityForTest() == originalEngine,
                "Rejected hot-swap should keep the active DT-CWPT engine instance");
        expect (guardedProcessor->getObservedMorphProcessorForTest() == originalMorphProcessor,
                "Rejected hot-swap should keep the active MorphBandProcessor instance");
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "Rejected hot-swap should emit one shared invalid-topology diagnostic");
        expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                      juce::String (topology::invalidTopologyDiagnosticCategory()),
                      "Rejected hot-swap should use the shared invalid-topology diagnostic category");
    }

    void testInvalidPersistedTopologyFallsBackToBuiltInDefault()
    {
        topology::resetInvalidTopologyDiagnosticsForTest();

        auto processor = std::make_unique<MorphAudioProcessor>();
        const juce::MemoryBlock invalidState = createStateWithTopologyProperty ("LLLLLLLLLLLLL");

        processor->setStateInformation (invalidState.getData(), static_cast<int> (invalidState.getSize()));
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);

        auto referenceProcessor = createPreparedProcessor();
        const auto mainSignal = makeSignal (kBlockSize, 293.66, 990.0, 587.33, 1480.0);

        const auto fallbackRun = processOnce (*processor, mainSignal, std::nullopt);
        const auto referenceRun = processOnce (*referenceProcessor, mainSignal, std::nullopt);

        const auto expectedDefaultTopology = topology::serializeTopologyDestinations (getDefaultTopology());

        expect (processor->getEngineIdentityForTest() != nullptr,
                "Processor should keep a valid engine after invalid restore fallback");
        expect (processor->getObservedMorphProcessorForTest() != nullptr,
                "Processor should keep a valid band processor after invalid restore fallback");
        expect (processor->getCommittedDestinationsForTest() == getDefaultTopology(),
                "Invalid persisted topology should fall back to the built-in default topology");
        expect (processor->getPersistedTopologyStringForTest() == expectedDefaultTopology,
                "Fallback should store the built-in default topology back into APVTS state");
        expectQuantizedOutputsEqual (referenceRun.output, fallbackRun.output,
                                     "Invalid restore fallback should remain behaviorally identical to a fresh default processor");
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "Invalid persisted topology should emit one shared invalid-topology diagnostic");
        expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                      juce::String (topology::invalidTopologyDiagnosticCategory()),
                      "Invalid persisted topology should emit the shared diagnostic category");
    }

    void testValidPersistedStateRoundTripsTopologyAndProcessorBehavior()
    {
        const auto topologyCandidate = TestUtils::getFullPacketDestinations (5);
        const auto expectedPersistedTopology = topology::serializeTopologyDestinations (topologyCandidate);
        const MorphParameterState parameterState { 0.44f, 0.74f, -66.0f, true, true };
        const auto mainSignal = makeSignal (kBlockSize, 180.0, 540.0, 360.0, 1080.0);
        const auto sidechainSignal = makeSignal (kBlockSize, 250.0, 750.0, 500.0, 1500.0);

        auto stateSource = createPreparedProcessor();
        expect (stateSource->applyTopologyCandidateForTest (topologyCandidate),
                "State source should accept the topology that will be serialized");
        applyParameterState (*stateSource, parameterState);
        const auto stateData = getStateInformationCopy (*stateSource);

        auto restoredProcessor = std::make_unique<MorphAudioProcessor>();
        restoredProcessor->setStateInformation (stateData.getData(), static_cast<int> (stateData.getSize()));
        restoredProcessor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);

        auto referenceProcessor = createPreparedProcessor();
        expect (referenceProcessor->applyTopologyCandidateForTest (topologyCandidate),
                "Reference processor should accept the topology used by the restored state");
        applyParameterState (*referenceProcessor, parameterState);

        const auto restoredRun = processOnce (*restoredProcessor, mainSignal, sidechainSignal);
        const auto referenceRun = processOnce (*referenceProcessor, mainSignal, sidechainSignal);

        expect (restoredProcessor->getCommittedDestinationsForTest() == topologyCandidate,
                "Restored processor should commit the topology stored in the persisted state");
        expectEquals (restoredProcessor->getPersistedTopologyStringForTest(), expectedPersistedTopology,
                      "Restored processor should round-trip the persisted topology string");
        expectQuantizedOutputsEqual (referenceRun.output, restoredRun.output,
                                     "Valid restored state should preserve observable processor behavior");
        expectEquals (restoredRun.latencySamples, referenceRun.latencySamples,
                      "Valid restored state should preserve committed latency");
        expectEquals (restoredRun.analyzerDelaySamples, referenceRun.analyzerDelaySamples,
                      "Valid restored state should preserve analyzer delay alignment");
    }

    void testBridgeSnapshotsStayLatencyAlignedAfterPrepareRebuildAndRestore()
    {
        auto preparedProcessor = createPreparedProcessor();
        expectLatencyAlignedSnapshot (*preparedProcessor, "Prepare flow");

        auto rebuiltProcessor = createPreparedProcessor();
        const auto rebuiltTopology = TestUtils::getFullPacketDestinations (4);
        expect (rebuiltProcessor->applyTopologyCandidateForTest (rebuiltTopology),
                "Rebuild scenario should accept the committed topology candidate");
        expectLatencyAlignedSnapshot (*rebuiltProcessor, "Rebuild flow");

        auto restoreSource = createPreparedProcessor();
        const auto restoredTopology = TestUtils::getWaveletTreeDestinations (5);
        expect (restoreSource->applyTopologyCandidateForTest (restoredTopology),
                "Restore source should accept the topology stored in state");
        const auto stateData = getStateInformationCopy (*restoreSource);

        auto restoredProcessor = std::make_unique<MorphAudioProcessor>();
        restoredProcessor->setStateInformation (stateData.getData(), static_cast<int> (stateData.getSize()));
        restoredProcessor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBlockSize);
        expect (restoredProcessor->getCommittedDestinationsForTest() == restoredTopology,
                "Restore flow should commit the topology stored in state before bridge observation");
        expectLatencyAlignedSnapshot (*restoredProcessor, "State-restore flow");
    }

    void testCommittedProcessBlockScenariosRemainAllocationFree()
    {
        auto noSidechainProcessor = createPreparedProcessor();
        auto duplicatedSidechainProcessor = createPreparedProcessor();
        const auto topologyCandidate = TestUtils::getFullPacketDestinations (6);
        const auto mainSignal = makeSignal (kBlockSize, 205.0, 615.0, 410.0, 1230.0);
        const auto sidechainSignal = makeSignal (kBlockSize, 155.0, 465.0, 310.0, 930.0);

        expect (noSidechainProcessor->applyTopologyCandidateForTest (topologyCandidate),
                "No-sidechain allocation scenario should accept the committed topology");
        expect (duplicatedSidechainProcessor->applyTopologyCandidateForTest (topologyCandidate),
                "Explicit-sidechain allocation scenario should accept the committed topology");

        auto noSidechainBuffer = makeProcessBuffer (mainSignal, std::nullopt);
        auto duplicatedSidechainBuffer = makeProcessBuffer (mainSignal, sidechainSignal);
        juce::MidiBuffer midi;

        TestUtils::AllocationCounter counter;
        counter.begin();

        for (int iteration = 0; iteration < 4; ++iteration)
        {
            noSidechainProcessor->processBlock (noSidechainBuffer, midi);
            duplicatedSidechainProcessor->processBlock (duplicatedSidechainBuffer, midi);
        }

        counter.end();

        expect (counter.getCount() == 0,
                "Committed Morph processBlock scenarios should remain allocation-free; found "
                    + juce::String (static_cast<int> (counter.getCount())) + " allocations");
        expect (noSidechainProcessor->getCommittedDestinationsForTest() == topologyCandidate,
                "Allocation-free no-sidechain coverage should preserve the committed topology");
        expect (duplicatedSidechainProcessor->getCommittedDestinationsForTest() == topologyCandidate,
                "Allocation-free explicit-sidechain coverage should preserve the committed topology");
    }

    void testMixedDepthCommittedMorphTopologyRemainsDeterministicUnderTheWorklistScheduler()
    {
        const auto mixedCases = RegressionFixtures::buildDeterministicTopologyMatrix (
            RegressionFixtures::TopologyFamily::mixedDepth,
            std::array<int, 1> { 12 },
            std::array<unsigned int, 1> { 701U });
        const auto& topologyCandidate = mixedCases.front ().destinations;
        const auto expectedPersistedTopology = topology::serializeTopologyDestinations (topologyCandidate);
        const MorphParameterState parameterState { 0.51f, 0.33f, -58.0f, false, true };
        const auto mainSignal = makeSignal (kBlockSize, 287.0, 987.0, 431.0, 1430.0);
        const auto sidechainSignal = makeSignal (kBlockSize, 199.0, 631.0, 389.0, 1217.0);

        auto firstProcessor = createPreparedProcessor();
        auto secondProcessor = createPreparedProcessor();

        expect (firstProcessor->applyTopologyCandidateForTest (topologyCandidate),
                "First processor should accept the deterministic mixed-depth topology");
        expect (secondProcessor->applyTopologyCandidateForTest (topologyCandidate),
                "Second processor should accept the same deterministic mixed-depth topology");

        applyParameterState (*firstProcessor, parameterState);
        applyParameterState (*secondProcessor, parameterState);

        const auto firstRun = processOnce (*firstProcessor, mainSignal, sidechainSignal);
        const auto secondRun = processOnce (*secondProcessor, mainSignal, sidechainSignal);

        expectQuantizedOutputsEqual (firstRun.output, secondRun.output,
                                     "Mixed-depth committed Morph runs should remain deterministic under the worklist scheduler");
        expect (firstRun.committedDestinations == topologyCandidate,
                "First Morph processor should preserve the committed mixed-depth topology");
        expect (secondRun.committedDestinations == topologyCandidate,
                "Second Morph processor should preserve the committed mixed-depth topology");
        expectEquals (firstRun.persistedTopology, expectedPersistedTopology,
                      "First Morph processor should persist the mixed-depth topology string");
        expectEquals (secondRun.persistedTopology, expectedPersistedTopology,
                      "Second Morph processor should persist the mixed-depth topology string");
        expectEquals (firstRun.latencySamples, secondRun.latencySamples,
                      "Mixed-depth committed Morph runs should preserve latency deterministically");
    }
};

static MorphAudioProcessorTests morphAudioProcessorTests;
