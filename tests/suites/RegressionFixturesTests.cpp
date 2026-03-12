#include "../util/RegressionFixtures.h"
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>

#include <array>
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

        beginTest("Reconstruction evaluation uses the c07 oracle threshold");
        testReconstructionEvaluationUsesTheC07OracleThreshold();
    }

private:
    using TopologyFamily = RegressionFixtures::TopologyFamily;
    using RandomTopologyCase = RegressionFixtures::RandomTopologyCase;
    using QuantizedBufferView = RegressionFixtures::QuantizedBufferView;
    using BandSnapshot = RegressionFixtures::BandSnapshot;
    using SnapshotSet = RegressionFixtures::SnapshotSet;

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

    void testReconstructionEvaluationUsesTheC07OracleThreshold()
    {
        const std::vector<double> input{1.0, -1.0};

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
};

static RegressionFixturesTests regressionFixturesTests;
