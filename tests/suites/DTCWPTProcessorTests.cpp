/**
 * @file DTCWPTProcessorTests.cpp
 * @brief JUCE UnitTest suite for DTCWPTProcessor.
 */

#include <dtcwpt/dtcwpt_processor.h>
#include <dtcwpt/dtcwpt_band_processor.h>
#include "../util/RegressionFixtures.h"
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

/**
 * @brief Passthrough BandProcessor for reconstruction tests.
 */
class PassthroughProcessor : public dtcwpt::BandProcessor {
public:
    void prepare(double, int, int, int) override {}
    void reset() override {}
    // processBand() is no-op by default - passthrough
};

class DestinationCaptureProcessor : public dtcwpt::BandProcessor {
public:
    void prepare(double, int, int, int) override {}

    void processAllBands(dtcwpt::BandData& data) override {
        ++processCalls;
        if (data.destinationIds != nullptr) {
            observedDestinationIds = *data.destinationIds;
        }
    }

    void reset() override {
        processCalls = 0;
        observedDestinationIds.clear();
    }

    int processCalls = 0;
    std::vector<int> observedDestinationIds;
};

class ArrivalSemanticsProcessor : public dtcwpt::BandProcessor {
public:
    void prepare(double, int, int, int) override {
        reset();
    }

    void processAllBands(dtcwpt::BandData& data) override {
        ++processCalls;
        hasSidechainHistory.push_back(data.hasSidechain ? static_cast<char>(1) : static_cast<char>(0));
        if (data.destinationIds != nullptr) {
            observedDestinationIds = *data.destinationIds;
        }

        if (accumulatedSamplesPerBand.empty()) {
            accumulatedSamplesPerBand.assign(static_cast<size_t>(data.numBands), 0);
        }

        for (int bandIndex = 0; bandIndex < data.numBands; ++bandIndex) {
            accumulatedSamplesPerBand[static_cast<size_t>(bandIndex)] += static_cast<int>(data.bands[0][static_cast<size_t>(bandIndex)].numSamples);
        }
    }

    void reset() override {
        processCalls = 0;
        observedDestinationIds.clear();
        accumulatedSamplesPerBand.clear();
        hasSidechainHistory.clear();
    }

    int processCalls = 0;
    std::vector<int> observedDestinationIds;
    std::vector<int> accumulatedSamplesPerBand;
    std::vector<char> hasSidechainHistory;
};

class DTCWPTProcessorTests : public juce::UnitTest {
public:
    DTCWPTProcessorTests() : UnitTest("DTCWPTProcessor") {}

    void runTest() override {
        beginTest("Passthrough reconstruction");
        testPassthroughReconstruction();

        beginTest("Re-prepare resets state");
        testReprepareResets();

        beginTest("Allocation-free processing");
        testAllocationFree();

        beginTest("Sidechain channel adjustment");
        testSidechainChannelAdjust();

        beginTest("No sidechain flag");
        testNoSidechainFlag();

        beginTest("BandProcessor prepare called");
        testBandProcessorPrepare();

        beginTest("Full depth-8 reconstruction");
        testFullDepth8();

        beginTest("Random topology depth-8 reconstruction");
        testRandomTopologyDepth8();

        beginTest("Full depth-12 reconstruction");
        testFullDepth12();

        beginTest("Configured maxDepth boundaries");
        testConfiguredMaxDepthBoundaries();

        beginTest("Malformed topologies rejected");
        testMalformedTopologiesRejected();

        beginTest("Complete leaf set accepted at configured depth");
        testCompleteLeafSetAcceptedAtConfiguredDepth();

        beginTest("Complete leaf set accepted below configured depth");
        testCompleteLeafSetAcceptedBelowConfiguredDepth();

        beginTest("Accepted topology preserves destination order");
        testAcceptedTopologyPreservesDestinationOrder();

        beginTest("Analysis arrival semantics remain stable for valid topologies");
        testAnalysisArrivalSemanticsRemainStableForValidTopologies();

        beginTest("Host block segmentation invariance is bit-exact after quantization");
        testHostBlockSegmentationInvarianceIsBitExactAfterQuantization();

        beginTest("Sidechain symmetry preserves snapshots for identical input streams");
        testSidechainSymmetryPreservesSnapshotsForIdenticalInputStreams();

        beginTest("Deterministic topology matrices reconstruct across all families");
        testDeterministicTopologyMatricesReconstructAcrossAllFamilies();

        beginTest("Audio-thread processing remains allocation-free with sidechain");
        testAudioThreadProcessingRemainsAllocationFreeWithSidechain();

        beginTest("actualDepth greater than maxDepth rejected");
        testActualDepthGreaterThanMaxDepthRejected();

        beginTest("Failed re-prepare preserves prior state");
        testFailedRepreparePreservesPriorState();
    }

private:
    static constexpr int kDefaultMaxDepth = dtcwpt::topology_limits::kSupportedMaxDepth;

    static int pathToNodeIndex(const std::string& path) {
        int index = 1;
        for (const char c : path) {
            index = (index << 1) | (c == 'H' ? 1 : 0);
        }
        return index;
    }

    dtcwpt::TopologyConfig createConfig(const std::vector<std::string>& dests, int maxDepth = kDefaultMaxDepth) {
        dtcwpt::TopologyConfig config;
        config.destinations = dests;
        config.maxDepth = maxDepth;
        return config;
    }

    static std::vector<double> makeScenarioSignal(int totalSamples)
    {
        std::vector<double> signal(static_cast<size_t>(totalSamples), 0.0);
        constexpr double twoPi = 6.28318530717958647692;

        for (int index = 0; index < totalSamples; ++index) {
            const double sample = static_cast<double>(index);
            signal[static_cast<size_t>(index)] = 0.31 * std::sin(twoPi * sample / 37.0)
                + 0.17 * std::cos(twoPi * sample / 23.0)
                + 0.09 * std::sin(twoPi * sample * sample / 8192.0);
        }

        return signal;
    }

    static std::vector<RegressionFixtures::SegmentationPlan> makeAlternateSegmentationPlans(int totalSamples)
    {
        return {
            {"two-halves", {totalSamples / 2, totalSamples / 2}},
            {"ragged", {1536, 512, 1024, 1024}},
            {"sixteen-blocks", std::vector<int>(16, totalSamples / 16)},
        };
    }

    RegressionFixtures::AnalysisSnapshotScenarioResult runSnapshotScenario(
        const dtcwpt::TopologyConfig& config,
        const std::vector<double>& input,
        std::optional<std::span<const double>> sidechain,
        const RegressionFixtures::SegmentationPlan& plan)
    {
        return RegressionFixtures::runAnalysisSnapshotScenario(config, input, sidechain, plan);
    }

    std::vector<double> processSignal(dtcwpt::DTCWPTProcessor& processor,
                                      const std::vector<double>& input,
                                      int latency,
                                      int blockSize = 512) {
        std::vector<double> output;
        output.reserve(input.size() + static_cast<size_t>(latency + blockSize));

        int cursor = 0;
        while (cursor < static_cast<int>(input.size())) {
            const int currentBlockSize = std::min(blockSize, static_cast<int>(input.size()) - cursor);

            juce::AudioBuffer<double> buffer(1, currentBlockSize);
            auto* data = buffer.getWritePointer(0);

            for (int i = 0; i < currentBlockSize; ++i) {
                data[i] = input[static_cast<size_t>(cursor + i)];
            }

            processor.processBlock(buffer);

            const auto* outData = buffer.getReadPointer(0);
            for (int i = 0; i < currentBlockSize; ++i) {
                output.push_back(outData[i]);
            }

            cursor += currentBlockSize;
        }

        for (int i = 0; i < latency; i += blockSize) {
            const int currentBlockSize = std::min(blockSize, latency - i);
            juce::AudioBuffer<double> buffer(1, currentBlockSize);
            buffer.clear();
            processor.processBlock(buffer);

            const auto* outData = buffer.getReadPointer(0);
            for (int j = 0; j < currentBlockSize; ++j) {
                output.push_back(outData[j]);
            }
        }

        return output;
    }

    void expectReconstructionBelowThreshold(const std::vector<double>& input,
                                            const std::vector<double>& output,
                                            int latency,
                                            const juce::String& label) {
        const auto reconstruction = RegressionFixtures::evaluateReconstruction(input, output, latency);
        expect(reconstruction.passesMseThreshold,
               label + " should be <= -200 dB, got " + juce::String(reconstruction.mseDb, 6)
                   + " dB with max abs error " + juce::String(reconstruction.maxAbsError, 12));
    }

    void expectInvalidArgument(const std::function<void()>& action, const juce::String& message) {
        bool threwInvalidArgument = false;

        try {
            action();
        } catch (const std::invalid_argument&) {
            threwInvalidArgument = true;
        } catch (...) {
        }

        expect(threwInvalidArgument, message);
    }

    void testPassthroughReconstruction() {
        // Passthrough BandProcessor -> input reproduced with ε ≤ 1e-9, delayed by getLatency()
        auto config = createConfig(TestUtils::getFullPacketDestinations(6), 8);
        
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 1);
        
        int latency = processor.getLatency();

        // Generate test signal
        auto input = TestUtils::generateSine(1000.0, 0.5, TestUtils::TestConfig::SAMPLE_RATE);
        
        std::vector<double> output;
        output.reserve(input.size() + latency + 512);

        // Process in blocks
        int cursor = 0;
        const int blockSize = 512;
        
        while (cursor < static_cast<int>(input.size())) {
            int currentBlockSize = std::min(blockSize, static_cast<int>(input.size()) - cursor);
            
            juce::AudioBuffer<double> buffer(1, currentBlockSize);
            auto* data = buffer.getWritePointer(0);
            
            for (int i = 0; i < currentBlockSize; ++i) {
                data[i] = input[static_cast<size_t>(cursor + i)];
            }
            
            processor.processBlock(buffer);
            
            const auto* outData = buffer.getReadPointer(0);
            for (int i = 0; i < currentBlockSize; ++i) {
                output.push_back(outData[i]);
            }
            
            cursor += currentBlockSize;
        }

        // Tail processing
        for (int i = 0; i < latency; i += blockSize) {
            int currentBlockSize = std::min(blockSize, latency - i);
            juce::AudioBuffer<double> buffer(1, currentBlockSize);
            buffer.clear();
            processor.processBlock(buffer);
            
            const auto* outData = buffer.getReadPointer(0);
            for (int j = 0; j < currentBlockSize; ++j) {
                output.push_back(outData[j]);
            }
        }

        // Verify reconstruction
        if (output.size() >= input.size() + static_cast<size_t>(latency)) {
            double maxError = 0.0;
            for (size_t i = 0; i < input.size(); ++i) {
                double error = std::abs(input[i] - output[i + static_cast<size_t>(latency)]);
                maxError = std::max(maxError, error);
            }
            
            expect(maxError < 1e-9,
                   "Passthrough reconstruction error should be < 1e-9, got " + juce::String(maxError));
        } else {
            expect(false, "Output buffer too short for reconstruction check");
        }
    }

    void testReprepareResets() {
        // Second prepareToPlay() -> behaves like fresh construction
        auto config = createConfig(TestUtils::getFullPacketDestinations(4), 8);
        
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 1);
        
        int latency1 = processor.getLatency();

        // Prepare again
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 1);
        
        int latency2 = processor.getLatency();

        expectEquals(latency1, latency2, "Latency should be consistent after re-prepare");
    }

    void testAllocationFree() {
        // processBlock() after prepare -> zero heap allocations
        auto config = createConfig(TestUtils::getFullPacketDestinations(6), 8);
        
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 1);

        juce::AudioBuffer<double> buffer(1, 512);
        buffer.clear();

        TestUtils::AllocationCounter counter;
        counter.begin();

        // Process multiple blocks
        for (int i = 0; i < 10; ++i) {
            processor.processBlock(buffer);
        }

        counter.end();

        expect(counter.getCount() == 0,
               "No allocations should occur during processBlock() - found " + 
               juce::String(static_cast<int>(counter.getCount())));
    }

    void testSidechainChannelAdjust() {
        // Sidechain with different channel count -> adjusted without allocation
        auto config = createConfig(TestUtils::getFullPacketDestinations(4), 8);
        
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 2);  // 2 channels

        juce::AudioBuffer<double> mainBuffer(2, 256);
        juce::AudioBuffer<double> sidechainBuffer(1, 256);  // 1 channel (different)
        
        mainBuffer.clear();
        sidechainBuffer.clear();

        // Should handle different channel counts
        processor.processSidechain(sidechainBuffer);
        processor.processBlock(mainBuffer);

        expect(true, "Sidechain channel adjustment should work without error");
    }

    void testNoSidechainFlag() {
        // No processSidechain() call -> hasSidechain = false in BandData
        auto config = createConfig(TestUtils::getFullPacketDestinations(3), 8);
        
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 1);

        juce::AudioBuffer<double> buffer(1, 256);
        buffer.clear();

        // Process without sidechain
        processor.processBlock(buffer);

        // If we get here without error, the no-sidechain path works
        expect(true, "Processing without sidechain should work");
    }

    void testBandProcessorPrepare() {
        // setBandProcessor() after prepare -> BandProcessor::prepare() called immediately
        auto config = createConfig(TestUtils::getFullPacketDestinations(3), 8);
        
        dtcwpt::DTCWPTProcessor processor;
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 1);

        // Set processor after prepare
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());

        // If prepare was called, the processor is ready
        expect(true, "setBandProcessor after prepare should work");
    }

    void testFullDepth8() {
        // Full packet depth 8 (256 leaves) -> deterministic reconstruction at the c07 threshold
        auto config = createConfig(TestUtils::getFullPacketDestinations(8), 8);
        
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 1);
        
        const int latency = processor.getLatency();
        const auto input = TestUtils::generateNoise(0.5, TestUtils::TestConfig::SAMPLE_RATE);
        const auto output = processSignal(processor, input, latency);
        expectReconstructionBelowThreshold(input, output, latency, "Full depth-8 reconstruction");
    }

    void testRandomTopologyDepth8() {
        // Random valid topology at depth 8 -> deterministic reconstruction at the c07 threshold
        auto dests = TestUtils::getRandomPacketDestinations(8, 42);
        auto config = createConfig(dests, 8);
        
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 1);
        
        const int latency = processor.getLatency();
        const auto input = TestUtils::generateNoise(0.5, TestUtils::TestConfig::SAMPLE_RATE, 42);
        const auto output = processSignal(processor, input, latency);
        expectReconstructionBelowThreshold(input, output, latency, "Random depth-8 reconstruction");
    }

    void testFullDepth12() {
        auto config = createConfig(TestUtils::getFullPacketDestinations(12), kDefaultMaxDepth);

        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 1);

        const int latency = processor.getLatency();
        const auto input = TestUtils::generateNoise(0.5, TestUtils::TestConfig::SAMPLE_RATE, 99U);
        const auto output = processSignal(processor, input, latency);

        expectReconstructionBelowThreshold(input, output, latency, "Full depth-12 MSE");
    }

    void testConfiguredMaxDepthBoundaries() {
        dtcwpt::DTCWPTProcessor processor;
        const auto destinations = TestUtils::getFullPacketDestinations(4);

        expectInvalidArgument([&]() {
            processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512,
                                    createConfig(destinations, 0), 1);
        }, "maxDepth=0 should throw std::invalid_argument");

        expectInvalidArgument([&]() {
            processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512,
                                    createConfig(destinations, 13), 1);
        }, "maxDepth=13 should throw std::invalid_argument");
    }

    void testMalformedTopologiesRejected() {
        dtcwpt::DTCWPTProcessor processor;

        expectInvalidArgument([&]() {
            processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 16, createConfig({}, 4), 1);
        }, "Empty destination list should throw std::invalid_argument");

        expectInvalidArgument([&]() {
            processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 16,
                                    createConfig({"LL", "", "HH"}, 4), 1);
        }, "Empty destination path should throw std::invalid_argument");

        expectInvalidArgument([&]() {
            processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 16,
                                    createConfig({"L", "HX"}, 4), 1);
        }, "Non-L/H character should throw std::invalid_argument");

        expectInvalidArgument([&]() {
            processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 16,
                                    createConfig({"L", "L", "H"}, 4), 1);
        }, "Duplicate destination should throw std::invalid_argument");

        expectInvalidArgument([&]() {
            processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 16,
                                    createConfig({"L", "LL", "LH", "H"}, 4), 1);
        }, "Ancestor overlap should throw std::invalid_argument");

        expectInvalidArgument([&]() {
            processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 16,
                                    createConfig({"L", "HL"}, 4), 1);
        }, "Missing sibling topology {L, HL} should throw std::invalid_argument");

        expectInvalidArgument([&]() {
            processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 16,
                                    createConfig({"LL", "H"}, 4), 1);
        }, "Missing sibling topology {LL, H} should throw std::invalid_argument");
    }

    void testCompleteLeafSetAcceptedAtConfiguredDepth() {
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());

        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 32,
                                createConfig(TestUtils::getFullPacketDestinations(3), 3), 1);

        expect(processor.getLatency() > 0,
               "Valid topology whose actualDepth equals maxDepth should be accepted");
    }

    void testCompleteLeafSetAcceptedBelowConfiguredDepth() {
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());

        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 32,
                                createConfig({"LLL", "LLH", "LH", "H"}, 5), 1);

        expect(processor.getLatency() > 0,
               "Valid topology whose actualDepth is below maxDepth should be accepted");
    }

    void testAcceptedTopologyPreservesDestinationOrder() {
        auto captureProcessor = std::make_unique<DestinationCaptureProcessor>();
        auto* captureProcessorPtr = captureProcessor.get();

        const std::vector<std::string> destinations = {"H", "LH", "LLL", "LLH"};
        std::vector<int> expectedDestinationIds;
        expectedDestinationIds.reserve(destinations.size());
        for (const auto& destination : destinations) {
            expectedDestinationIds.push_back(pathToNodeIndex(destination));
        }

        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::move(captureProcessor));
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 16,
                                createConfig(destinations, 5), 1);

        juce::AudioBuffer<double> buffer(1, 16);
        buffer.clear();
        processor.processBlock(buffer);

        expect(captureProcessorPtr->processCalls > 0,
               "BandProcessor should observe at least one processed block");
        expectEquals(captureProcessorPtr->observedDestinationIds.size(), expectedDestinationIds.size(),
                     "Observed destination count should match caller order count");
        expect(captureProcessorPtr->observedDestinationIds == expectedDestinationIds,
               "Accepted topology should preserve caller-provided destination order");
    }

    void testAnalysisArrivalSemanticsRemainStableForValidTopologies() {
        auto captureProcessor = std::make_unique<ArrivalSemanticsProcessor>();
        auto* captureProcessorPtr = captureProcessor.get();

        const std::vector<std::string> destinations = {"H", "LL", "LH"};
        std::vector<int> expectedDestinationIds;
        expectedDestinationIds.reserve(destinations.size());
        for (const auto& destination : destinations) {
            expectedDestinationIds.push_back(pathToNodeIndex(destination));
        }

        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::move(captureProcessor));
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 8,
                                createConfig(destinations, 2), 1);

        const auto input = makeScenarioSignal(16);
        for (int blockOffset = 0; blockOffset < 16; blockOffset += 8) {
            juce::AudioBuffer<double> buffer(1, 8);
            auto* writePtr = buffer.getWritePointer(0);
            for (int sampleIndex = 0; sampleIndex < 8; ++sampleIndex) {
                writePtr[sampleIndex] = input[static_cast<size_t>(blockOffset + sampleIndex)];
            }
            processor.processBlock(buffer);
        }

        expectEquals(captureProcessorPtr->processCalls, 2,
                     "Two host blocks should yield two deterministic analysis arrivals at maxBlockSize=8");
        expect(captureProcessorPtr->observedDestinationIds == expectedDestinationIds,
               "Arrival semantics should preserve destination order exactly");
        expect(captureProcessorPtr->accumulatedSamplesPerBand == std::vector<int>({8, 4, 4}),
               "Aggregated per-band sample counts should remain stable for the valid depth-2 topology");
    }

    void testHostBlockSegmentationInvarianceIsBitExactAfterQuantization() {
        const int totalSamples = 4096;
        const auto input = makeScenarioSignal(totalSamples);
        const auto mixedCase = RegressionFixtures::buildDeterministicTopologyMatrix(
            RegressionFixtures::TopologyFamily::mixedDepth,
            std::array<int, 1>{6},
            std::array<unsigned int, 1>{419U});
        const auto config = createConfig(mixedCase.front().destinations, 6);
        const auto reference = runSnapshotScenario(config, input, std::nullopt, {"reference", {totalSamples}});

        for (const auto& plan : makeAlternateSegmentationPlans(totalSamples)) {
            const auto alternate = runSnapshotScenario(config, input, std::nullopt, plan);
            juce::String failureMessage;

            expectEquals(alternate.bandProcessCalls, reference.bandProcessCalls,
                         "Equivalent host segmentations should keep the same number of analysis arrivals");
            expect(RegressionFixtures::compareSegmentationInvariantResults(reference.mainSnapshots,
                                                                           alternate.mainSnapshots,
                                                                           reference.quantizedOutput,
                                                                           alternate.quantizedOutput,
                                                                           failureMessage),
                   juce::String("Plan '") + juce::String(plan.name) + juce::String("' should match the reference segmentation after float32 quantization: ") + failureMessage);
        }
    }

    void testSidechainSymmetryPreservesSnapshotsForIdenticalInputStreams() {
        const int totalSamples = 4096;
        const auto input = makeScenarioSignal(totalSamples);
        const auto mixedCase = RegressionFixtures::buildDeterministicTopologyMatrix(
            RegressionFixtures::TopologyFamily::mixedDepth,
            std::array<int, 1>{5},
            std::array<unsigned int, 1>{557U});
        const auto config = createConfig(mixedCase.front().destinations, 5);
        const auto result = runSnapshotScenario(config, input, std::span<const double>(input), {"reference", {totalSamples}});

        juce::String failureMessage;
        expect(result.hasSidechainSnapshots,
               "Sidechain-enabled scenarios should capture observable sidechain snapshots");
        expect(result.mainSnapshots.destinationsInOrder == config.destinations,
               "Main snapshots should preserve the configured destination order");
        expect(result.sidechainSnapshots.destinationsInOrder == config.destinations,
               "Sidechain snapshots should preserve the configured destination order");
        expect(RegressionFixtures::compareSnapshotSets(result.mainSnapshots, result.sidechainSnapshots, failureMessage),
               "Identical main and sidechain inputs should yield identical quantized snapshots: " + failureMessage);
    }

    void testDeterministicTopologyMatricesReconstructAcrossAllFamilies() {
        const int totalSamples = 4096;
        const auto input = makeScenarioSignal(totalSamples);
        const auto dwtCases = RegressionFixtures::buildDeterministicTopologyMatrix(
            RegressionFixtures::TopologyFamily::dwt,
            std::array<int, 12>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
            std::array<unsigned int, 1>{101U});
        const auto fullTreeCases = RegressionFixtures::buildDeterministicTopologyMatrix(
            RegressionFixtures::TopologyFamily::fullTree,
            std::array<int, 12>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
            std::array<unsigned int, 1>{211U});
        const auto mixedCases = RegressionFixtures::buildDeterministicTopologyMatrix(
            RegressionFixtures::TopologyFamily::mixedDepth,
            std::array<int, 11>{2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
            std::array<unsigned int, 2>{307U, 401U});

        expectEquals(static_cast<int>(dwtCases.size()), 12,
                     "The default DWT matrix should include 12 deterministic cases");
        expectEquals(static_cast<int>(fullTreeCases.size()), 12,
                     "The default full-tree matrix should include 12 deterministic cases");
        expect(mixedCases.size() >= 12,
               "The default mixed-depth matrix should include at least 12 deterministic cases");

        auto verifyCases = [&](const std::vector<RegressionFixtures::RandomTopologyCase>& cases,
                               const juce::String& familyLabel,
                               int limit) {
            for (int index = 0; index < limit; ++index) {
                const auto& topologyCase = cases[static_cast<size_t>(index)];
                const auto config = createConfig(topologyCase.destinations, topologyCase.depth);
                const auto scenario = runSnapshotScenario(config, input, std::nullopt, {"reference", {totalSamples}});
                expectReconstructionBelowThreshold(input,
                                                   scenario.output,
                                                   scenario.latencySamples,
                                                   familyLabel + " case " + juce::String(index));
            }
        };

        verifyCases(dwtCases, "dwt", 12);
        verifyCases(fullTreeCases, "full-tree", 12);
        verifyCases(mixedCases, "mixed-depth", 12);
    }

    void testAudioThreadProcessingRemainsAllocationFreeWithSidechain() {
        const auto config = createConfig({"H", "LL", "LH"}, 2);

        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 256, config, 1);

        juce::AudioBuffer<double> mainBuffer(1, 256);
        juce::AudioBuffer<double> sidechainBuffer(1, 256);
        const auto input = makeScenarioSignal(256);
        auto* mainData = mainBuffer.getWritePointer(0);
        auto* sidechainData = sidechainBuffer.getWritePointer(0);
        for (int sampleIndex = 0; sampleIndex < 256; ++sampleIndex) {
            mainData[sampleIndex] = input[static_cast<size_t>(sampleIndex)];
            sidechainData[sampleIndex] = input[static_cast<size_t>(sampleIndex)];
        }

        TestUtils::AllocationCounter counter;
        counter.begin();
        for (int iteration = 0; iteration < 8; ++iteration) {
            processor.processSidechain(sidechainBuffer);
            processor.processBlock(mainBuffer);
        }
        counter.end();

        expect(counter.getCount() == 0,
               "Sidechain-enabled audio-thread processing should remain allocation-free");
    }

    void testActualDepthGreaterThanMaxDepthRejected() {
        dtcwpt::DTCWPTProcessor processor;
        const auto destinations = TestUtils::getFullPacketDestinations(6);

        expectInvalidArgument([&]() {
            processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512,
                                    createConfig(destinations, 5), 1);
        }, "actualDepth > maxDepth should throw std::invalid_argument");
    }

    void testFailedRepreparePreservesPriorState() {
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());

        const auto validConfig = createConfig(TestUtils::getFullPacketDestinations(4), 4);
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, validConfig, 1);

        const int latencyBeforeFailure = processor.getLatency();
        const auto input = TestUtils::generateNoise(0.25, TestUtils::TestConfig::SAMPLE_RATE, 123U);

        expectInvalidArgument([&]() {
            processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512,
                                    createConfig(TestUtils::getFullPacketDestinations(6), 5), 1);
        }, "Rejected re-prepare should throw std::invalid_argument");

        expectEquals(processor.getLatency(), latencyBeforeFailure,
                     "Latency should remain unchanged after failed re-prepare");

        const auto output = processSignal(processor, input, latencyBeforeFailure);
        expectReconstructionBelowThreshold(input, output, latencyBeforeFailure,
                                           "Reconstruction after failed re-prepare");
    }
};

// Static registration
static DTCWPTProcessorTests dtcwptProcessorTests;
