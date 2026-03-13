#include "../util/RegressionFixtures.h"
#include "../util/AnalysisArrivalOracle.h"
#include "../util/TestUtils.h"

#include <dtcwpt/dtcwpt_processor.h>

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <vector>

class RegressionFixturesTests : public juce::UnitTest {
public:
    RegressionFixturesTests()
        : juce::UnitTest("RegressionFixtures")
    {
    }

    void runTest() override
    {
        beginTest("Deterministic topology matrices are stable and valid");
        testDeterministicTopologyMatricesAreStableAndValid();

        beginTest("Deterministic topology matrices cover all families");
        testDeterministicTopologyMatricesCoverAllFamilies();

        beginTest("Float32 quantization is stable and preserves metadata");
        testFloat32QuantizationIsStableAndPreservesMetadata();

        beginTest("Quantized buffer comparisons fail on mismatches");
        testQuantizedBufferComparisonsFailOnMismatches();

        beginTest("Snapshot comparisons check structure before values");
        testSnapshotComparisonsCheckStructureBeforeValues();

        beginTest("Segmentation invariance checks structure before output values");
        testSegmentationInvarianceChecksStructureBeforeOutputValues();

        beginTest("Analysis snapshot scenarios preserve input destination order and sample metadata");
        testAnalysisSnapshotScenariosPreserveInputDestinationOrderAndSampleMetadata();

        beginTest("Analysis snapshot scenarios return quantized data ready for segmentation comparison");
        testAnalysisSnapshotScenariosReturnQuantizedDataReadyForSegmentationComparison();

        beginTest("Reconstruction evaluation uses the c07 oracle threshold");
        testReconstructionEvaluationUsesTheC07OracleThreshold();

        beginTest("Trace comparison uses strict structure and tolerant sample values");
        testTraceComparisonUsesStrictStructureAndTolerantSampleValues();

        beginTest("Linear scan baseline preserves destination order and mixed topology trace semantics");
        testLinearScanBaselinePreservesDestinationOrderAndMixedTopologyTraceSemantics();

        beginTest("Linear scan baseline uses legacy adapter expected call counts");
        testLinearScanBaselineUsesLegacyAdapterExpectedCallCounts();

        beginTest("Per-destination output comparison checks structure and value tolerance");
        testPerDestinationOutputComparisonChecksStructureAndValueTolerance();

        beginTest("Scheduler equivalence scenario helper matches baseline for mixed topology");
        testSchedulerEquivalenceScenarioHelperMatchesBaselineForMixedTopology();
    }

private:
    using TopologyFamily = RegressionFixtures::TopologyFamily;
    using RandomTopologyCase = RegressionFixtures::RandomTopologyCase;
    using QuantizedBufferView = RegressionFixtures::QuantizedBufferView;
    using BandSnapshot = RegressionFixtures::BandSnapshot;
    using SnapshotSet = RegressionFixtures::SnapshotSet;
    using AnalysisSnapshotScenarioResult = RegressionFixtures::AnalysisSnapshotScenarioResult;

    static SnapshotSet makeSnapshotSet()
    {
        SnapshotSet set;
        set.destinationsInOrder = {"LL", "H"};
        set.samplesPerBandInOrder = {2, 1};
        set.bands = {
            BandSnapshot{"LL", 2, 1, QuantizedBufferView{1, 2, {0.25f, -0.5f}}, QuantizedBufferView{1, 2, {0.0f, 0.125f}}},
            BandSnapshot{"H", 1, 1, QuantizedBufferView{1, 1, {0.75f}}, QuantizedBufferView{1, 1, {-0.25f}}}
        };
        return set;
    }

    static dtcwpt::TopologyConfig makeConfig(const std::vector<std::string>& destinations, int maxDepth)
    {
        dtcwpt::TopologyConfig config;
        config.destinations = destinations;
        config.maxDepth = maxDepth;
        return config;
    }

    static std::vector<double> makeScenarioSignal(int totalSamples)
    {
        std::vector<double> signal(static_cast<size_t>(totalSamples), 0.0);
        constexpr double twoPi = 6.28318530717958647692;

        for (int index = 0; index < totalSamples; ++index) {
            const double sample = static_cast<double>(index);
            signal[static_cast<size_t>(index)] = 0.37 * std::sin(twoPi * sample / 11.0)
                + 0.19 * std::cos(twoPi * sample / 7.0);
        }

        return signal;
    }

    static AnalysisSnapshotScenarioResult runScenario(const RegressionFixtures::SegmentationPlan& plan)
    {
        const auto input = makeScenarioSignal(16);
        const auto config = makeConfig({"H", "LL", "LH"}, 2);
        return RegressionFixtures::runAnalysisSnapshotScenario(config, input, std::nullopt, plan);
    }

    static void expectCasesEqual(const std::vector<RandomTopologyCase>& expected,
                                 const std::vector<RandomTopologyCase>& actual,
                                 juce::UnitTest& owner)
    {
        owner.expectEquals(static_cast<int>(actual.size()), static_cast<int>(expected.size()),
                           "Topology matrix size should be stable");

        if (actual.size() != expected.size()) {
            return;
        }

        for (size_t index = 0; index < expected.size(); ++index) {
            owner.expect(static_cast<int>(expected[index].family) == static_cast<int>(actual[index].family),
                         "Topology family should be stable at index " + juce::String(static_cast<int>(index)));
            owner.expectEquals(actual[index].depth, expected[index].depth,
                               "Topology depth should be stable at index " + juce::String(static_cast<int>(index)));
            owner.expectEquals(static_cast<int>(actual[index].seed), static_cast<int>(expected[index].seed),
                               "Topology seed should be stable at index " + juce::String(static_cast<int>(index)));
            owner.expect(actual[index].destinations == expected[index].destinations,
                         "Topology destinations should be stable at index " + juce::String(static_cast<int>(index)));
        }
    }

    void testDeterministicTopologyMatricesAreStableAndValid()
    {
        constexpr std::array<int, 3> depths{2, 3, 4};
        constexpr std::array<unsigned int, 4> seeds{11U, 29U, 47U, 83U};

        const auto first = RegressionFixtures::buildDeterministicTopologyMatrix(TopologyFamily::mixedDepth, depths, seeds);
        const auto second = RegressionFixtures::buildDeterministicTopologyMatrix(TopologyFamily::mixedDepth, depths, seeds);

        expectCasesEqual(first, second, *this);
        expectEquals(static_cast<int>(first.size()), 12, "Mixed-depth matrix should include one case per depth/seed pair");

        for (const auto& topologyCase : first) {
            expect(TestUtils::validateDestinations(topologyCase.destinations),
                   "Each deterministic topology case should be valid");
        }
    }

    void testDeterministicTopologyMatricesCoverAllFamilies()
    {
        constexpr std::array<int, 3> depths{2, 3, 4};
        constexpr std::array<unsigned int, 4> seeds{5U, 13U, 17U, 31U};

        const auto dwtCases = RegressionFixtures::buildDeterministicTopologyMatrix(TopologyFamily::dwt, depths, seeds);
        const auto fullTreeCases = RegressionFixtures::buildDeterministicTopologyMatrix(TopologyFamily::fullTree, depths, seeds);
        const auto mixedDepthCases = RegressionFixtures::buildDeterministicTopologyMatrix(TopologyFamily::mixedDepth, depths, seeds);

        expect(dwtCases.size() >= 12, "DWT family should provide at least 12 deterministic cases");
        expect(fullTreeCases.size() >= 12, "Full-tree family should provide at least 12 deterministic cases");
        expect(mixedDepthCases.size() >= 12, "Mixed-depth family should provide at least 12 deterministic cases");

        for (const auto& topologyCase : dwtCases) {
            expect(topologyCase.destinations == TestUtils::getWaveletTreeDestinations(topologyCase.depth),
                   "DWT family should match the canonical wavelet tree topology");
        }

        for (const auto& topologyCase : fullTreeCases) {
            expect(topologyCase.destinations == TestUtils::getFullPacketDestinations(topologyCase.depth),
                   "Full-tree family should match the canonical full packet topology");
        }

        for (const auto& topologyCase : mixedDepthCases) {
            const auto fullTree = TestUtils::getFullPacketDestinations(topologyCase.depth);
            const auto waveletTree = TestUtils::getWaveletTreeDestinations(topologyCase.depth);

            expect(topologyCase.destinations != fullTree,
                   "Mixed-depth family should not collapse to a full tree case");
            expect(topologyCase.destinations != waveletTree,
                   "Mixed-depth family should not collapse to a DWT case");
        }
    }

    void testFloat32QuantizationIsStableAndPreservesMetadata()
    {
        const std::vector<double> source{0.1, 0.2, 0.3, -0.4, -0.5, -0.6};

        const auto quantizedA = RegressionFixtures::quantizeToFloat32(source, 2, 3);
        const auto quantizedB = RegressionFixtures::quantizeToFloat32(source, 2, 3);

        expectEquals(quantizedA.numChannels, 2, "Quantized view should preserve channel count");
        expectEquals(quantizedA.numSamples, 3, "Quantized view should preserve sample count");
        expect(quantizedA.interleaved == quantizedB.interleaved,
               "Repeated quantization should be stable");
        expectEquals(static_cast<int>(quantizedA.interleaved.size()), 6,
                     "Quantized view should preserve total sample layout");

        expect(quantizedA.interleaved == std::vector<float>({0.1f, 0.2f, 0.3f, -0.4f, -0.5f, -0.6f}),
               "Quantization should convert values to explicit float32 samples");
    }

    void testQuantizedBufferComparisonsFailOnMismatches()
    {
        const QuantizedBufferView reference{2, 2, {0.1f, 0.2f, 0.3f, 0.4f}};

        juce::String failureMessage;
        expect(! RegressionFixtures::compareQuantizedBuffers(reference, QuantizedBufferView{1, 2, {0.1f, 0.2f}}, failureMessage),
               "Channel count mismatches should fail");
        expect(failureMessage.contains("channel") || failureMessage.contains("size"),
               "Channel mismatch should produce a structural failure message");

        failureMessage.clear();
        expect(! RegressionFixtures::compareQuantizedBuffers(reference, QuantizedBufferView{2, 2, {0.1f, 0.2f, 0.3f, 0.5f}}, failureMessage),
               "Value mismatches should fail");
        expect(failureMessage.contains("sample") || failureMessage.contains("value"),
               "Value mismatch should identify the differing sample");

        failureMessage.clear();
        expect(RegressionFixtures::compareQuantizedBuffers(reference, reference, failureMessage),
               "Bit-exact equal quantized buffers should pass");
        expect(failureMessage.isEmpty(), "Successful buffer comparison should not report a failure message");
    }

    void testSnapshotComparisonsCheckStructureBeforeValues()
    {
        const auto reference = makeSnapshotSet();

        auto reordered = reference;
        reordered.destinationsInOrder = {"H", "LL"};
        reordered.bands[0].destination = "H";
        reordered.bands[1].destination = "LL";

        juce::String failureMessage;
        expect(! RegressionFixtures::compareSnapshotSets(reference, reordered, failureMessage),
               "Destination order mismatches should fail");
        expect(failureMessage.contains("destination"),
               "Destination order mismatch should report structural failure details");

        auto wrongCounts = reference;
        wrongCounts.samplesPerBandInOrder[1] = 2;
        wrongCounts.bands[1].numSamples = 2;
        wrongCounts.bands[1].real.numSamples = 2;
        wrongCounts.bands[1].imag.numSamples = 2;
        wrongCounts.bands[1].real.interleaved.push_back(0.0f);
        wrongCounts.bands[1].imag.interleaved.push_back(0.0f);

        failureMessage.clear();
        expect(! RegressionFixtures::compareSnapshotSets(reference, wrongCounts, failureMessage),
               "Per-band sample count mismatches should fail");
        expect(failureMessage.contains("sample"),
               "Sample count mismatch should report structural failure details");

        failureMessage.clear();
        expect(RegressionFixtures::compareSnapshotSets(reference, reference, failureMessage),
               "Matching snapshot sets should pass");
        expect(failureMessage.isEmpty(), "Successful snapshot comparison should not report a failure message");
    }

    void testSegmentationInvarianceChecksStructureBeforeOutputValues()
    {
        const auto referenceSnapshots = makeSnapshotSet();
        auto alternateSnapshots = referenceSnapshots;
        alternateSnapshots.samplesPerBandInOrder[0] = 3;
        alternateSnapshots.bands[0].numSamples = 3;
        alternateSnapshots.bands[0].real.numSamples = 3;
        alternateSnapshots.bands[0].imag.numSamples = 3;
        alternateSnapshots.bands[0].real.interleaved.push_back(1.0f);
        alternateSnapshots.bands[0].imag.interleaved.push_back(1.0f);

        const QuantizedBufferView referenceOutput{1, 2, {0.0f, 1.0f}};
        const QuantizedBufferView alternateOutput{1, 2, {0.0f, 999.0f}};

        juce::String failureMessage;
        expect(! RegressionFixtures::compareSegmentationInvariantResults(
            referenceSnapshots,
            alternateSnapshots,
            referenceOutput,
            alternateOutput,
            failureMessage),
            "Structural mismatches should fail segmentation invariance checks");
        expect(failureMessage.contains("sample") || failureMessage.contains("destination"),
               "Segmentation invariance should report snapshot structure mismatches before value mismatches");
        expect(! failureMessage.contains("999"),
               "Structure mismatches should short-circuit before output value comparison details");
    }

    void testAnalysisSnapshotScenariosPreserveInputDestinationOrderAndSampleMetadata()
    {
        const auto result = runScenario({"segmented", {5, 3, 8}});

        expectEquals(result.bandProcessCalls, 1,
                     "Shared snapshot scenarios should capture one aggregated analysis arrival for the full reference window");
        expect(result.mainSnapshots.destinationsInOrder == std::vector<std::string>({"H", "LL", "LH"}),
               "Captured destinations should preserve the caller-provided input order");
        expectEquals(static_cast<int>(result.mainSnapshots.samplesPerBandInOrder.size()),
                     static_cast<int>(result.mainSnapshots.bands.size()),
                     "Per-band sample counts should align with captured bands");

        for (size_t bandIndex = 0; bandIndex < result.mainSnapshots.bands.size(); ++bandIndex) {
            const auto& band = result.mainSnapshots.bands[bandIndex];
            expectEquals(result.mainSnapshots.samplesPerBandInOrder[bandIndex], band.numSamples,
                         "Per-band sample metadata should match the captured snapshot length");
            expectEquals(band.real.numSamples, band.numSamples,
                         "Real snapshots should track the captured band sample count");
            expectEquals(band.imag.numSamples, band.numSamples,
                         "Imaginary snapshots should track the captured band sample count");
            expectEquals(static_cast<int>(band.real.interleaved.size()), band.numChannels * band.numSamples,
                         "Real snapshots should already be interleaved and quantized");
            expectEquals(static_cast<int>(band.imag.interleaved.size()), band.numChannels * band.numSamples,
                         "Imaginary snapshots should already be interleaved and quantized");
        }
    }

    void testAnalysisSnapshotScenariosReturnQuantizedDataReadyForSegmentationComparison()
    {
        const auto reference = runScenario({"reference", {16}});
        const auto alternate = runScenario({"alternate", {4, 4, 4, 4}});

        juce::String failureMessage;
        expect(RegressionFixtures::compareSegmentationInvariantResults(reference.mainSnapshots,
                                                                       alternate.mainSnapshots,
                                                                       reference.quantizedOutput,
                                                                       alternate.quantizedOutput,
                                                                       failureMessage),
               "Captured scenario data should be directly comparable without an extra quantization pass");
        expect(failureMessage.isEmpty(),
               "Segmentation comparison should succeed cleanly for equivalent already-quantized captures");
    }

    void testReconstructionEvaluationUsesTheC07OracleThreshold()
    {
        const std::vector<double> input{1.0, -1.0};

        expect(RegressionFixtures::kReconstructionMseThresholdDb != TestUtils::TestConfig::MSE_THRESHOLD_DB,
               "The c07 reconstruction oracle must remain distinct from the legacy helper threshold");

        constexpr double kThresholdEdgeError = 9.9e-11;
        const std::vector<double> exactThresholdOutput{0.0, 1.0 + kThresholdEdgeError, -1.0 + kThresholdEdgeError};
        const auto exactThreshold = RegressionFixtures::evaluateReconstruction(input, exactThresholdOutput, 1);
        expectWithinAbsoluteError(exactThreshold.mseDb, -200.0, 0.25,
                                  "A 1e-10 error should land on the locked -200 dB threshold");
        expectWithinAbsoluteError(exactThreshold.maxAbsError, kThresholdEdgeError, 1.0e-15,
                                  "Max absolute error should track the largest aligned sample delta");
        expect(exactThreshold.passesMseThreshold,
               "The c07 reconstruction oracle should pass exactly at -200 dB");

        const std::vector<double> nearLegacyThresholdOutput{0.0, 1.0 + 1.0e-5, -1.0 + 1.0e-5};
        const auto nearLegacyThreshold = RegressionFixtures::evaluateReconstruction(input, nearLegacyThresholdOutput, 1);
        expect(nearLegacyThreshold.mseDb < TestUtils::TestConfig::MSE_THRESHOLD_DB,
               "The chosen error should still beat the legacy -80 dB helper threshold");
        expect(! nearLegacyThreshold.passesMseThreshold,
               "The c07 reconstruction helper must not reuse the legacy -80 dB threshold");
    }

    void testTraceComparisonUsesStrictStructureAndTolerantSampleValues()
    {
        const std::vector<dtcwpt::AnalysisTraceEvent> reference{
            {dtcwpt::AnalysisTraceEventKind::dequeue, dtcwpt::AnalysisPathId::mainReal, 0, 0, 1, 0, 0, 0.25, true},
            {dtcwpt::AnalysisTraceEventKind::destinationWrite, dtcwpt::AnalysisPathId::mainReal, 0, 0, 2, 0, 0, -0.5, false},
        };

        auto tolerant = reference;
        tolerant[0].sampleValue += 5.0e-16;

        juce::String failureMessage;
        expect(RegressionFixtures::compareAnalysisTraces(reference, tolerant, failureMessage),
               "Trace comparison should tolerate sampleValue differences within 1e-15");
        expect(failureMessage.isEmpty(),
               "Successful trace comparison should not report a failure message");

        auto wrongNode = reference;
        wrongNode[1].nodeId = 7;
        failureMessage.clear();
        expect(! RegressionFixtures::compareAnalysisTraces(reference, wrongNode, failureMessage),
               "Trace comparison should reject structural mismatches");
        expect(failureMessage.contains("node ID"),
               "Structural mismatch should report the field that differed");

        auto wrongValue = reference;
        wrongValue[0].sampleValue += 1.0e-12;
        failureMessage.clear();
        expect(! RegressionFixtures::compareAnalysisTraces(reference, wrongValue, failureMessage),
               "Trace comparison should reject sampleValue differences above 1e-15");
        expect(failureMessage.contains("sample value"),
               "Sample mismatch should report a numeric payload failure");
    }

    void testLinearScanBaselinePreservesDestinationOrderAndMixedTopologyTraceSemantics()
    {
        const auto config = makeConfig({"L", "HL", "HH"}, 2);
        const std::vector<double> input{0.25};

        dtcwpt::test::resetLegacyAdapterCallCount();
        const auto result = RegressionFixtures::runLinearScanBaseline(
            config,
            input,
            dtcwpt::AnalysisPathId::mainReal);

        expect(result.destinationIdsInOrder == std::vector<int>({2, 6, 7}),
               "Baseline should preserve destination IDs in exact input order");
        expectEquals(static_cast<int>(result.perDestinationOutputs.size()), 3,
                     "Baseline should return one output buffer per destination in order");

        expectEquals(static_cast<int>(result.trace.size()), 9,
                     "Mixed topology one-sample baseline should emit dequeue, child-arrival, and destination-write events deterministically");
        expect(result.trace[0].kind == dtcwpt::AnalysisTraceEventKind::dequeue && result.trace[0].nodeId == 1,
               "Root dequeue must be the first trace event");
        expect(result.trace[3].kind == dtcwpt::AnalysisTraceEventKind::dequeue && result.trace[3].nodeId == 3,
               "Internal H dequeue must occur after the root child-arrival events");

        expect(result.trace[1].kind == dtcwpt::AnalysisTraceEventKind::childArrivalRecorded,
               "Left child arrival should be recorded immediately after root dequeue");
        expectEquals(result.trace[1].relatedNodeId, 2,
                     "Left child arrival should target node 2");
        expect(! result.trace[1].enteredFrontier,
               "Leaf child L should be recorded without entering the frontier");

        expect(result.trace[2].kind == dtcwpt::AnalysisTraceEventKind::childArrivalRecorded,
               "Right child arrival should be recorded immediately after left child arrival");
        expectEquals(result.trace[2].relatedNodeId, 3,
                     "Right child arrival should target node 3");
        expect(result.trace[2].enteredFrontier,
               "Internal child H should be recorded as entering the frontier");
        expect(result.trace[6].kind == dtcwpt::AnalysisTraceEventKind::destinationWrite
                   && result.trace[6].nodeId == 2,
               "Leaf L destination write should occur after internal processing completes");
    }

    void testLinearScanBaselineUsesLegacyAdapterExpectedCallCounts()
    {
        {
            const auto config = makeConfig({"L", "HL", "HH"}, 2);
            const std::vector<double> input{0.5};

            dtcwpt::test::resetLegacyAdapterCallCount();
            (void) RegressionFixtures::runLinearScanBaseline(config, input, dtcwpt::AnalysisPathId::mainReal);
            expectEquals(dtcwpt::test::getLegacyAdapterCallCount(), 2,
                         "Mixed topology baseline must invoke the legacy adapter exactly for nodes 1 and 3");
        }

        {
            const auto config = makeConfig({"LL", "LH", "HL", "HH"}, 2);
            const std::vector<double> input{0.5};

            dtcwpt::test::resetLegacyAdapterCallCount();
            (void) RegressionFixtures::runLinearScanBaseline(config, input, dtcwpt::AnalysisPathId::mainReal);
            expectEquals(dtcwpt::test::getLegacyAdapterCallCount(), 3,
                         "Depth-2 full tree baseline must invoke the legacy adapter exactly for nodes 1, 2, and 3");
        }
    }

    void testPerDestinationOutputComparisonChecksStructureAndValueTolerance()
    {
        const std::vector<int> destinationIds{2, 6, 7};
        const std::vector<std::vector<double>> reference{{0.25, 0.5}, {0.75}, {-0.125}};

        auto tolerant = reference;
        tolerant[0][1] += 5.0e-16;

        juce::String failureMessage;
        expect(RegressionFixtures::comparePerDestinationOutputs(destinationIds, reference,
                                                                destinationIds, tolerant,
                                                                failureMessage),
               "Per-destination output comparison should tolerate <= 1e-15 numeric drift");
        expect(failureMessage.isEmpty(),
               "Successful per-destination output comparison should not report a failure message");

        auto wrongOrder = destinationIds;
        std::swap(wrongOrder[0], wrongOrder[1]);
        failureMessage.clear();
        expect(! RegressionFixtures::comparePerDestinationOutputs(destinationIds, reference,
                                                                 wrongOrder, reference,
                                                                 failureMessage),
               "Per-destination output comparison should reject destination-order mismatches");
        expect(failureMessage.contains("destination order"),
               "Destination-order mismatch should be reported structurally");

        auto wrongValue = reference;
        wrongValue[1][0] += 1.0e-12;
        failureMessage.clear();
        expect(! RegressionFixtures::comparePerDestinationOutputs(destinationIds, reference,
                                                                 destinationIds, wrongValue,
                                                                 failureMessage),
               "Per-destination output comparison should reject values above 1e-15 tolerance");
        expect(failureMessage.contains("sample value"),
               "Numeric output mismatch should report the offending sample");
    }

    void testSchedulerEquivalenceScenarioHelperMatchesBaselineForMixedTopology()
    {
        const auto config = makeConfig({"L", "HL", "HH"}, 2);
        const std::vector<double> input{0.25, 0.0, 0.0, 0.0};

        const auto scenario = RegressionFixtures::runSchedulerEquivalenceScenario(
            config,
            input,
            std::span<const double>(input),
            dtcwpt::AnalysisPathId::mainReal);

        juce::String traceFailure;
        expect(RegressionFixtures::compareAnalysisTraces(scenario.baseline.trace,
                                                         scenario.productionTrace,
                                                         traceFailure),
               "Scheduler equivalence helper should align production and baseline traces: " + traceFailure);

        juce::String outputFailure;
        expect(RegressionFixtures::comparePerDestinationOutputs(scenario.baseline.destinationIdsInOrder,
                                                                scenario.baseline.perDestinationOutputs,
                                                                scenario.productionDestinationIdsInOrder,
                                                                scenario.productionPerDestinationOutputs,
                                                                outputFailure),
               "Scheduler equivalence helper should align production and baseline per-destination outputs: " + outputFailure);
    }
};

static RegressionFixturesTests regressionFixturesTests;
