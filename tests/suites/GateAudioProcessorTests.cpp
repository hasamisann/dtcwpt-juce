#include "GateAudioProcessor.h"

#include "../../sample_plugins/common/InvalidTopologyDiagnostics.h"
#include "../../sample_plugins/common/TopologyPersistence.h"
#include "../util/RegressionFixtures.h"
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

class GateAudioProcessorTests : public juce::UnitTest
{
public:
    GateAudioProcessorTests() : juce::UnitTest ("GateAudioProcessorTests") {}

    void runTest() override
    {
        beginTest ("Threshold fanout and lowest-band bypass stay observable at processor level");
        testThresholdFanoutAndLowestBandBypass();

        beginTest ("Invalid hot-swap preserves committed topology and invalid restore falls back");
        testInvalidHotSwapPreservesCommittedTopologyAndRestoreFallsBack();

        beginTest ("Valid persisted-state restore roundtrips topology and behavior");
        testValidPersistedStateRestoreRoundtrip();

        beginTest ("Latency-aligned analyzer and bridge observations stay consistent");
        testLatencyAlignedBridgeObservationsStayConsistent();

        beginTest ("Gate processBlock remains allocation-free for covered committed-state paths");
        testProcessBlockRemainsAllocationFree();

        beginTest ("Fresh Gate processors remain deterministic from identical starting state");
        testFreshProcessorsRemainDeterministic();

        beginTest ("Mixed-depth committed Gate topology remains deterministic under the worklist scheduler");
        testMixedDepthCommittedGateTopologyRemainsDeterministicUnderTheWorklistScheduler();

        beginTest ("Gate rejects valid core topology beyond depth-8 limit");
        testGateRejectsValidCoreTopologyBeyondDepthEightLimit();
    }

private:
    struct ProcessorObservation
    {
        std::vector<std::string> committedDestinations;
        juce::String persistedTopology;
        int latencySamples = 0;
        int analyzerInputDelaySamples = 0;
        const GateBandProcessor* observedGateProcessor = nullptr;
        const dtcwpt::DTCWPTProcessor* engineIdentity = nullptr;
    };

    static constexpr int kDefaultBlockSize = 512;
    static constexpr int kBridgeSnapshotSamples = SpectrumDataBridge::kSlotSize;

    static const std::vector<std::string>& getDefaultTopology()
    {
        static const std::vector<std::string> kDefaultTopology
            { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" };
        return kDefaultTopology;
    }

    static std::unique_ptr<GateAudioProcessor> createPreparedProcessor (int blockSize = kDefaultBlockSize)
    {
        auto processor = std::make_unique<GateAudioProcessor>();
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, blockSize);
        return processor;
    }

    static ProcessorObservation captureObservation (const GateAudioProcessor& processor)
    {
        return {
            processor.getCommittedDestinationsForTest(),
            processor.getPersistedTopologyStringForTest(),
            processor.getLatencySamples(),
            processor.getAnalyzerInputDelaySamplesForTest(),
            processor.getObservedGateProcessorForTest(),
            processor.getEngineIdentityForTest()
        };
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

    static juce::MemoryBlock captureState (GateAudioProcessor& processor)
    {
        juce::MemoryBlock state;
        processor.getStateInformation (state);
        return state;
    }

    static std::vector<float> toFloatSignal (const std::vector<double>& source, float gain = 1.0f)
    {
        std::vector<float> signal;
        signal.reserve (source.size());

        for (const double sample : source)
            signal.push_back (static_cast<float> (sample * static_cast<double> (gain)));

        return signal;
    }

    static std::vector<float> makeLowFrequencySignal (int numSamples)
    {
        const double duration = static_cast<double> (numSamples) / TestUtils::TestConfig::SAMPLE_RATE;
        return toFloatSignal (TestUtils::generateSine (110.0, duration, TestUtils::TestConfig::SAMPLE_RATE), 1.8f);
    }

    static std::vector<float> makeConstantSignal (int numSamples, float value)
    {
        return std::vector<float> (static_cast<std::size_t> (numSamples), value);
    }

    static std::vector<float> makeDeterministicSignal (int numSamples)
    {
        const double duration = static_cast<double> (numSamples) / TestUtils::TestConfig::SAMPLE_RATE;
        const auto low = TestUtils::generateSine (110.0, duration, TestUtils::TestConfig::SAMPLE_RATE);
        const auto high = TestUtils::generateSine (1760.0, duration, TestUtils::TestConfig::SAMPLE_RATE);
        const auto noise = TestUtils::generateNoise (duration, TestUtils::TestConfig::SAMPLE_RATE, 1337U);

        std::vector<float> signal (static_cast<std::size_t> (numSamples), 0.0f);

        for (int index = 0; index < numSamples; ++index)
        {
            const auto sampleIndex = static_cast<std::size_t> (index);
            const double sample = (low[sampleIndex] * 0.9) + (high[sampleIndex] * 0.3) + (noise[sampleIndex] * 0.1);
            signal[sampleIndex] = static_cast<float> (sample);
        }

        return signal;
    }

    static std::vector<float> makeImpulseSignal (int numSamples, int impulseIndex = 0)
    {
        auto impulse = TestUtils::generateImpulse (numSamples, impulseIndex);
        return toFloatSignal (impulse);
    }

    static juce::AudioBuffer<float> makeStereoBuffer (const std::vector<float>& mono)
    {
        juce::AudioBuffer<float> buffer (2, static_cast<int> (mono.size()));

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            buffer.copyFrom (channel, 0, mono.data(), static_cast<int> (mono.size()));

        return buffer;
    }

    static void copyBufferContents (juce::AudioBuffer<float>& destination, const juce::AudioBuffer<float>& source)
    {
        jassert (destination.getNumChannels() == source.getNumChannels());
        jassert (destination.getNumSamples() == source.getNumSamples());

        for (int channel = 0; channel < source.getNumChannels(); ++channel)
            destination.copyFrom (channel, 0, source, channel, 0, source.getNumSamples());
    }

    static juce::AudioBuffer<float> processMonoSignal (GateAudioProcessor& processor, const std::vector<float>& mono)
    {
        auto buffer = makeStereoBuffer (mono);
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
        return buffer;
    }

    static double computeRms (const juce::AudioBuffer<float>& buffer)
    {
        double sumSquares = 0.0;
        int sampleCount = 0;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const double value = static_cast<double> (buffer.getSample (channel, sample));
                sumSquares += value * value;
                ++sampleCount;
            }
        }

        return sampleCount > 0 ? std::sqrt (sumSquares / static_cast<double> (sampleCount)) : 0.0;
    }

    static RegressionFixtures::QuantizedBufferView quantizeBuffer (const juce::AudioBuffer<float>& buffer)
    {
        std::vector<double> interleaved;
        interleaved.reserve (static_cast<std::size_t> (buffer.getNumChannels() * buffer.getNumSamples()));

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                interleaved.push_back (static_cast<double> (buffer.getSample (channel, sample)));
        }

        return RegressionFixtures::quantizeToFloat32 (interleaved, buffer.getNumChannels(), buffer.getNumSamples());
    }

    static std::vector<float> readBandLevels (GateAudioProcessor& processor)
    {
        std::array<float, BandLevelBridge::kMaxBands> levels {};
        const int count = processor.getBandLevelBridge().read (levels.data());

        return std::vector<float> (levels.begin(), levels.begin() + count);
    }

    static int findPeakIndex (const std::array<float, SpectrumDataBridge::kSlotSize>& samples)
    {
        float peakMagnitude = 0.0f;
        int peakIndex = -1;

        for (int index = 0; index < static_cast<int> (samples.size()); ++index)
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

    static void setThresholdDb (GateAudioProcessor& processor, int bandIndex, float thresholdDb)
    {
        const auto parameterId = juce::String::formatted ("threshold_%02d", bandIndex);
        auto* parameter = dynamic_cast<juce::AudioParameterFloat*> (processor.getAPVTS().getParameter (parameterId));
        jassert (parameter != nullptr);

        if (parameter != nullptr)
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (thresholdDb));
    }

    static void setThresholdsDb (GateAudioProcessor& processor, const std::vector<float>& thresholdsDb)
    {
        for (int index = 0; index < static_cast<int> (thresholdsDb.size()); ++index)
            setThresholdDb (processor, index, thresholdsDb[static_cast<std::size_t> (index)]);
    }

    static void setBypassLowestBand (GateAudioProcessor& processor, bool shouldBypass)
    {
        auto* parameter = dynamic_cast<juce::AudioParameterBool*> (processor.getAPVTS().getParameter ("bypass_lowest_band"));
        jassert (parameter != nullptr);

        if (parameter != nullptr)
            parameter->setValueNotifyingHost (shouldBypass ? 1.0f : 0.0f);
    }

    void expectQuantizedOutputsMatch (const juce::AudioBuffer<float>& expected,
                                      const juce::AudioBuffer<float>& actual,
                                      const juce::String& context)
    {
        const auto expectedQuantized = quantizeBuffer (expected);
        const auto actualQuantized = quantizeBuffer (actual);

        juce::String failureMessage;
        const bool matches = RegressionFixtures::compareQuantizedBuffers (expectedQuantized, actualQuantized, failureMessage);
        expect (matches, context + ": " + failureMessage);
    }

    void expectLatencyAlignedObservation (GateAudioProcessor& processor, const juce::String& context)
    {
        const auto observation = captureObservation (processor);

        expectEquals (observation.analyzerInputDelaySamples, observation.latencySamples,
                      context + ": analyzer delay should match committed processor latency");

        const int targetSnapshotIndex = observation.latencySamples / kBridgeSnapshotSamples;
        const int expectedPeakIndex = observation.latencySamples % kBridgeSnapshotSamples;
        int observedPeakIndex = -1;

        for (int snapshotIndex = 0; snapshotIndex <= targetSnapshotIndex; ++snapshotIndex)
        {
            const auto blockSignal = snapshotIndex == 0
                ? makeImpulseSignal (kBridgeSnapshotSamples)
                : makeConstantSignal (kBridgeSnapshotSamples, 0.0f);

            auto processed = processMonoSignal (processor, blockSignal);
            ignoreUnused (processed);

            std::array<float, SpectrumDataBridge::kSlotSize> inputSnapshot {};
            std::array<float, SpectrumDataBridge::kSlotSize> outputSnapshot {};
            const bool hasSnapshot = processor.getSpectrumBridge().tryConsume (inputSnapshot, outputSnapshot);

            expect (hasSnapshot,
                    context + ": spectrum bridge should expose one consumable snapshot per full-slot process call");

            if (snapshotIndex == targetSnapshotIndex && hasSnapshot)
                observedPeakIndex = findPeakIndex (inputSnapshot);
        }

        expectEquals (observedPeakIndex, expectedPeakIndex,
                      context + ": analyzer snapshot should place the delayed impulse at the committed latency offset");

        const auto levels = readBandLevels (processor);
        expectEquals (static_cast<int> (levels.size()), static_cast<int> (observation.committedDestinations.size()),
                      context + ": band-level bridge count should follow the committed topology");
    }

    void testThresholdFanoutAndLowestBandBypass()
    {
        const auto twoBandTopology = TestUtils::getFullPacketDestinations (1);
        const auto signal = makeConstantSignal (kDefaultBlockSize, 0.8f);

        auto lowBandClosed = createPreparedProcessor();
        auto highBandClosed = createPreparedProcessor();
        auto lowBandBypassed = createPreparedProcessor();

        expect (lowBandClosed->applyTopologyCandidateForTest (twoBandTopology));
        expect (highBandClosed->applyTopologyCandidateForTest (twoBandTopology));
        expect (lowBandBypassed->applyTopologyCandidateForTest (twoBandTopology));

        setThresholdsDb (*lowBandClosed, { 10.0f, -120.0f });
        setThresholdsDb (*highBandClosed, { -120.0f, 10.0f });
        setThresholdsDb (*lowBandBypassed, { 10.0f, -120.0f });
        setBypassLowestBand (*lowBandBypassed, true);

        const auto lowBandClosedOutput = processMonoSignal (*lowBandClosed, signal);
        const auto highBandClosedOutput = processMonoSignal (*highBandClosed, signal);
        const auto lowBandBypassedOutput = processMonoSignal (*lowBandBypassed, signal);

        expectWithinAbsoluteError (lowBandClosed->getLastThresholdLinearForTest (0),
                                   std::pow (10.0, 10.0 / 20.0),
                                   1.0e-12,
                                   "The processor should fan out the first threshold parameter to the active GateBandProcessor");
        expectWithinAbsoluteError (lowBandClosed->getLastThresholdLinearForTest (1),
                                   std::pow (10.0, -120.0 / 20.0),
                                   1.0e-12,
                                   "The processor should fan out the second threshold parameter independently");
        expect (! lowBandClosed->getLastBypassLowestForTest(),
                "Lowest-band bypass should remain disabled when the bypass parameter is off");
        expect (lowBandBypassed->getLastBypassLowestForTest(),
                "Lowest-band bypass should be observable on the committed Gate processor when enabled");

        const auto observation = captureObservation (*lowBandBypassed);
        expect (observation.committedDestinations == twoBandTopology,
                "Threshold fanout coverage should preserve the committed two-band topology");

        const auto levels = readBandLevels (*lowBandBypassed);
        expectEquals (static_cast<int> (levels.size()), 2,
                      "Band-level bridge should expose one level per committed band after processing");
    }

    void testInvalidHotSwapPreservesCommittedTopologyAndRestoreFallsBack()
    {
        auto processor = createPreparedProcessor();
        const auto validCandidate = TestUtils::getFullPacketDestinations (8);
        expect (processor->applyTopologyCandidateForTest (validCandidate));

        const auto baselineObservation = captureObservation (*processor);

        topology::resetInvalidTopologyDiagnosticsForTest();
        processor->getTopologyState().requestChange ({ "L", "HL" });

        auto buffer = makeStereoBuffer (makeDeterministicSignal (kDefaultBlockSize));
        juce::MidiBuffer midi;
        processor->processBlock (buffer, midi);

        const auto afterInvalidHotSwap = captureObservation (*processor);

        expect (afterInvalidHotSwap.committedDestinations == baselineObservation.committedDestinations,
                "Rejected hot-swap should preserve committed destinations");
        expectEquals (afterInvalidHotSwap.persistedTopology, baselineObservation.persistedTopology,
                      "Rejected hot-swap should preserve persisted topology");
        expectEquals (afterInvalidHotSwap.latencySamples, baselineObservation.latencySamples,
                      "Rejected hot-swap should preserve reported latency");
        expectEquals (afterInvalidHotSwap.analyzerInputDelaySamples, baselineObservation.analyzerInputDelaySamples,
                      "Rejected hot-swap should preserve analyzer delay alignment");
        expect (afterInvalidHotSwap.observedGateProcessor == baselineObservation.observedGateProcessor,
                "Rejected hot-swap should preserve the committed GateBandProcessor instance");
        expect (afterInvalidHotSwap.engineIdentity == baselineObservation.engineIdentity,
                "Rejected hot-swap should preserve the committed engine instance");
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "Rejected hot-swap should emit the shared invalid-topology diagnostic once");
        expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                      juce::String (topology::invalidTopologyDiagnosticCategory()),
                      "Rejected hot-swap should report the shared invalid-topology diagnostic category");

        topology::resetInvalidTopologyDiagnosticsForTest();

        auto restored = std::make_unique<GateAudioProcessor>();
        const auto invalidState = createStateWithTopology (juce::String { "L,HL" });
        restored->setStateInformation (invalidState.getData(), static_cast<int> (invalidState.getSize()));
        restored->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kDefaultBlockSize);

        const auto restoredObservation = captureObservation (*restored);

        expect (restoredObservation.committedDestinations == getDefaultTopology(),
                "Invalid initial restore should fall back to the built-in default topology");
        expectEquals (restoredObservation.persistedTopology,
                      topology::serializeTopologyDestinations (getDefaultTopology()),
                      "Invalid initial restore should rewrite the built-in default topology string");
        expect (restoredObservation.observedGateProcessor != nullptr,
                "Fallback prepare should commit a usable GateBandProcessor instance");
        expect (restoredObservation.engineIdentity != nullptr,
                "Fallback prepare should commit a usable engine instance");
        expectEquals (restoredObservation.analyzerInputDelaySamples, restoredObservation.latencySamples,
                      "Fallback prepare should keep analyzer delay aligned to the committed latency");
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "Invalid initial restore should emit the shared invalid-topology diagnostic once");
        expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                      juce::String (topology::invalidTopologyDiagnosticCategory()),
                      "Invalid initial restore should report the shared invalid-topology diagnostic category");
    }

    void testValidPersistedStateRestoreRoundtrip()
    {
        auto reference = createPreparedProcessor();
        const auto validTopology = TestUtils::getFullPacketDestinations (4);
        const auto serializedTopology = topology::serializeTopologyDestinations (validTopology);

        expect (reference->applyTopologyCandidateForTest (validTopology));
        setThresholdsDb (*reference, { -6.0f, -18.0f, -36.0f, -48.0f });
        setBypassLowestBand (*reference, true);

        const auto state = captureState (*reference);
        auto restored = std::make_unique<GateAudioProcessor>();
        restored->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        restored->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kDefaultBlockSize);

        const auto referenceObservation = captureObservation (*reference);
        const auto restoredObservation = captureObservation (*restored);

        expect (restoredObservation.committedDestinations == validTopology,
                "Valid restore should commit the persisted topology");
        expectEquals (restoredObservation.persistedTopology, serializedTopology,
                      "Valid restore should roundtrip the persisted topology string");
        expectEquals (restoredObservation.latencySamples, referenceObservation.latencySamples,
                      "Valid restore should preserve committed latency");
        expectEquals (restoredObservation.analyzerInputDelaySamples, referenceObservation.analyzerInputDelaySamples,
                      "Valid restore should preserve analyzer delay alignment");

        const auto signal = makeDeterministicSignal (kDefaultBlockSize);
        const auto referenceOutput = processMonoSignal (*reference, signal);
        const auto restoredOutput = processMonoSignal (*restored, signal);

        expectQuantizedOutputsMatch (referenceOutput, restoredOutput,
                                     "Valid restore should preserve observable processor output");

        const auto referenceLevels = readBandLevels (*reference);
        const auto restoredLevels = readBandLevels (*restored);

        expectEquals (static_cast<int> (referenceLevels.size()), static_cast<int> (restoredLevels.size()),
                      "Valid restore should preserve band-level bridge width");
        expect (referenceLevels == restoredLevels,
                "Valid restore should preserve observable band-level bridge values for the same signal");
    }

    void testLatencyAlignedBridgeObservationsStayConsistent()
    {
        auto preparedDefault = createPreparedProcessor (kBridgeSnapshotSamples);
        expectLatencyAlignedObservation (*preparedDefault, "default prepare");

        auto rebuilt = createPreparedProcessor (kBridgeSnapshotSamples);
        const auto rebuiltTopology = TestUtils::getFullPacketDestinations (4);
        expect (rebuilt->applyTopologyCandidateForTest (rebuiltTopology));
        expectLatencyAlignedObservation (*rebuilt, "after topology rebuild");

        auto restoreSource = createPreparedProcessor (kBridgeSnapshotSamples);
        const auto restoredTopology = TestUtils::getWaveletTreeDestinations (8);
        expect (restoreSource->applyTopologyCandidateForTest (restoredTopology));
        setThresholdsDb (*restoreSource, { -12.0f, -24.0f, -48.0f });
        const auto restoreState = captureState (*restoreSource);

        auto restored = std::make_unique<GateAudioProcessor>();
        restored->setStateInformation (restoreState.getData(), static_cast<int> (restoreState.getSize()));
        restored->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, kBridgeSnapshotSamples);

        const auto sourceObservation = captureObservation (*restoreSource);
        const auto restoredObservation = captureObservation (*restored);
        expectEquals (restoredObservation.latencySamples, sourceObservation.latencySamples,
                      "State restore should preserve committed latency before bridge observation checks");
        expectLatencyAlignedObservation (*restored, "after valid state restore");
    }

    void testProcessBlockRemainsAllocationFree()
    {
        auto processor = createPreparedProcessor();
        expect (processor->applyTopologyCandidateForTest (TestUtils::getFullPacketDestinations (4)));
        setThresholdsDb (*processor, { -12.0f, -18.0f, -24.0f, -30.0f });
        setBypassLowestBand (*processor, false);

        const auto signal = makeDeterministicSignal (kDefaultBlockSize);
        const auto templateBuffer = makeStereoBuffer (signal);
        auto workingBuffer = makeStereoBuffer (signal);
        juce::MidiBuffer midi;

        TestUtils::AllocationCounter counter;
        counter.begin();

        for (int iteration = 0; iteration < 8; ++iteration)
        {
            copyBufferContents (workingBuffer, templateBuffer);
            processor->processBlock (workingBuffer, midi);
        }

        counter.end();

        expectEquals (static_cast<int> (counter.getCount()), 0,
                      "Covered Gate processBlock path should not allocate after prepare and commit");
    }

    void testFreshProcessorsRemainDeterministic()
    {
        const auto topology = TestUtils::getWaveletTreeDestinations (8);
        const std::vector<float> thresholds { -9.0f, -15.0f, -21.0f, -27.0f, -33.0f };
        const auto signal = makeDeterministicSignal (kDefaultBlockSize);

        auto first = createPreparedProcessor();
        auto second = createPreparedProcessor();

        expect (first->applyTopologyCandidateForTest (topology));
        expect (second->applyTopologyCandidateForTest (topology));

        setThresholdsDb (*first, thresholds);
        setThresholdsDb (*second, thresholds);
        setBypassLowestBand (*first, true);
        setBypassLowestBand (*second, true);

        const auto firstOutput = processMonoSignal (*first, signal);
        const auto secondOutput = processMonoSignal (*second, signal);

        const auto firstObservation = captureObservation (*first);
        const auto secondObservation = captureObservation (*second);

        expect (firstObservation.committedDestinations == secondObservation.committedDestinations,
                "Fresh processors should commit the same topology from identical starting state");
        expectEquals (firstObservation.persistedTopology, secondObservation.persistedTopology,
                      "Fresh processors should persist the same topology string from identical starting state");
        expectEquals (firstObservation.latencySamples, secondObservation.latencySamples,
                      "Fresh processors should report the same committed latency from identical starting state");
        expectEquals (firstObservation.analyzerInputDelaySamples, secondObservation.analyzerInputDelaySamples,
                      "Fresh processors should align analyzer delay the same way from identical starting state");

        expectQuantizedOutputsMatch (firstOutput, secondOutput,
                                     "Fresh processors should produce identical plugin-visible output for the same signal");

        const auto firstLevels = readBandLevels (*first);
        const auto secondLevels = readBandLevels (*second);
        expectEquals (static_cast<int> (firstLevels.size()), static_cast<int> (secondLevels.size()),
                      "Fresh processors should expose the same band-level bridge width");
        expect (firstLevels == secondLevels,
                "Fresh processors should expose identical band-level bridge values for the same signal");
    }

    void testMixedDepthCommittedGateTopologyRemainsDeterministicUnderTheWorklistScheduler()
    {
        const auto mixedCases = RegressionFixtures::buildDeterministicTopologyMatrix (
            RegressionFixtures::TopologyFamily::mixedDepth,
            std::array<int, 1> { 8 },
            std::array<unsigned int, 1> { 809U });
        const auto& topology = mixedCases.front ().destinations;
        const std::vector<float> thresholds { -10.0f, -16.0f, -22.0f, -28.0f, -34.0f };
        const auto signal = makeDeterministicSignal (kDefaultBlockSize);

        auto first = createPreparedProcessor();
        auto second = createPreparedProcessor();

        expect (first->applyTopologyCandidateForTest (topology));
        expect (second->applyTopologyCandidateForTest (topology));

        setThresholdsDb (*first, thresholds);
        setThresholdsDb (*second, thresholds);
        setBypassLowestBand (*first, false);
        setBypassLowestBand (*second, false);

        const auto firstOutput = processMonoSignal (*first, signal);
        const auto secondOutput = processMonoSignal (*second, signal);

        const auto firstObservation = captureObservation (*first);
        const auto secondObservation = captureObservation (*second);

        expect (firstObservation.committedDestinations == topology,
                "First Gate processor should preserve the committed mixed-depth topology");
        expect (secondObservation.committedDestinations == topology,
                "Second Gate processor should preserve the committed mixed-depth topology");
        expectEquals (firstObservation.persistedTopology, secondObservation.persistedTopology,
                      "Mixed-depth Gate processors should persist the same topology string");
        expectEquals (firstObservation.latencySamples, secondObservation.latencySamples,
                      "Mixed-depth Gate processors should preserve latency deterministically");

        expectQuantizedOutputsMatch (firstOutput, secondOutput,
                                     "Mixed-depth committed Gate runs should remain deterministic under the worklist scheduler");
    }

    void testGateRejectsValidCoreTopologyBeyondDepthEightLimit()
    {
        auto processor = createPreparedProcessor();
        const auto baselineObservation = captureObservation (*processor);
        const auto validDepthNineCandidate = TestUtils::getFullPacketDestinations (9);

        topology::resetInvalidTopologyDiagnosticsForTest();
        const bool applied = processor->applyTopologyCandidateForTest (validDepthNineCandidate);

        const auto rejectedObservation = captureObservation (*processor);

        expect (! applied, "Gate should reject topologies beyond its current depth-8 caller limit");
        expect (rejectedObservation.committedDestinations == baselineObservation.committedDestinations,
                "Depth-limit rejection must preserve committed topology");
        expectEquals (rejectedObservation.persistedTopology, baselineObservation.persistedTopology,
                      "Depth-limit rejection must preserve persisted topology");
        expectEquals (rejectedObservation.latencySamples, baselineObservation.latencySamples,
                      "Depth-limit rejection must preserve latency");
        expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1,
                      "Depth-limit rejection should emit the shared invalid-topology diagnostic");
        expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                      juce::String (topology::invalidTopologyDiagnosticCategory()),
                      "Depth-limit rejection should use the shared invalid-topology diagnostic category");
    }
};

static GateAudioProcessorTests gateAudioProcessorTests;
