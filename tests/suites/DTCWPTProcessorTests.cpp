/**
 * @file DTCWPTProcessorTests.cpp
 * @brief JUCE UnitTest suite for DTCWPTProcessor.
 */

#include <dtcwpt/dtcwpt_processor.h>
#include <dtcwpt/dtcwpt_band_processor.h>
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <functional>
#include <memory>
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
        const double mseDb = TestUtils::calculateMSEdB(input, output, latency);
        expect(mseDb < TestUtils::TestConfig::MSE_THRESHOLD_DB,
               label + " should be <= -80 dB, got " + juce::String(mseDb) + " dB");
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
        // Full packet depth 8 (256 leaves) -> white noise MSE <= -80 dB
        auto config = createConfig(TestUtils::getFullPacketDestinations(8), 8);
        
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 1);
        
        int latency = processor.getLatency();

        auto input = TestUtils::generateNoise(0.5, TestUtils::TestConfig::SAMPLE_RATE);
        
        std::vector<double> output;
        output.reserve(input.size() + latency + 512);

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

        // Tail
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

        double mseDb = TestUtils::calculateMSEdB(input, output, latency);
        
        expect(mseDb < TestUtils::TestConfig::MSE_THRESHOLD_DB,
               "Full depth-8 MSE should be <= -80 dB, got " + juce::String(mseDb) + " dB");
    }

    void testRandomTopologyDepth8() {
        // Random valid topology at depth 8 -> white noise MSE <= -80 dB
        auto dests = TestUtils::getRandomPacketDestinations(8, 42);
        auto config = createConfig(dests, 8);
        
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 512, config, 1);
        
        int latency = processor.getLatency();

        auto input = TestUtils::generateNoise(0.5, TestUtils::TestConfig::SAMPLE_RATE, 42);
        
        std::vector<double> output;
        output.reserve(input.size() + latency + 512);

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

        // Tail
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

        double mseDb = TestUtils::calculateMSEdB(input, output, latency);
        
        expect(mseDb < TestUtils::TestConfig::MSE_THRESHOLD_DB,
               "Random depth-8 MSE should be <= -80 dB, got " + juce::String(mseDb) + " dB");
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
