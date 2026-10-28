/**
 * @file StreamingEngineTests.cpp
 * @brief Latency and per-callback work distribution of DTCWPTProcessor.
 *
 * The engine must report a latency that depends only on the topology (the
 * filter-path delay), not on the host block size, and it must spread its work
 * evenly over host callbacks instead of doing a whole internal block at once.
 */

#include <dtcwpt/dtcwpt_processor.h>
#include <dtcwpt/dtcwpt_band_processor.h>
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

class PassthroughBandProcessor : public dtcwpt::BandProcessor {
public:
    void prepare(double, int, int, int) override {}
    void reset() override {}
};

dtcwpt::TopologyConfig makeConfig(const std::vector<std::string>& destinations, int maxDepth)
{
    dtcwpt::TopologyConfig config;
    config.destinations = destinations;
    config.maxDepth = maxDepth;
    return config;
}

struct NamedTopology {
    std::string name;
    dtcwpt::TopologyConfig config;
};

std::vector<NamedTopology> makeLatencyTopologies()
{
    return {
        {"6-stage DWT", makeConfig(TestUtils::getWaveletTreeDestinations(6), 6)},
        {"12-stage DWT", makeConfig(TestUtils::getWaveletTreeDestinations(12), 12)},
        {"depth-8 full packet", makeConfig(TestUtils::getFullPacketDestinations(8), 8)},
        {"depth-3 full packet", makeConfig(TestUtils::getFullPacketDestinations(3), 3)},
    };
}

} // namespace

class StreamingEngineTests : public juce::UnitTest {
public:
    StreamingEngineTests() : UnitTest("StreamingEngine") {}

    void runTest() override {
        beginTest("Latency does not depend on the host block size");
        testLatencyIndependentOfBlockSize();

        beginTest("Per-callback work is evenly distributed");
        testWorkIsEvenlyDistributed();
    }

private:
    static constexpr double kSampleRate = 48000.0;

    static int latencyFor(const dtcwpt::TopologyConfig& config, int maxBlockSize)
    {
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughBandProcessor>());
        processor.prepareToPlay(kSampleRate, maxBlockSize, config, 2);
        return processor.getLatency();
    }

    void testLatencyIndependentOfBlockSize()
    {
        for (const auto& topology : makeLatencyTopologies()) {
            const int reference = latencyFor(topology.config, 32);
            for (const int blockSize : {1, 64, 1024, 4096}) {
                expectEquals(latencyFor(topology.config, blockSize), reference,
                             juce::String(topology.name) + ": latency at maxBlockSize "
                                 + juce::String(blockSize) + " should equal the one at 32");
            }
        }
    }

#if DTCWPT_ENABLE_TEST_SEAMS
    struct WorkStats {
        double average = 0.0;
        std::uint64_t maximum = 0;
        int callbacks = 0;
    };

    // Runs noise through the engine in fixed host blocks and records, for every
    // callback, the number of analysis arrivals plus synthesis output samples.
    static WorkStats measureWork(const dtcwpt::TopologyConfig& config, int blockSize, int totalSamples)
    {
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughBandProcessor>());
        processor.prepareToPlay(kSampleRate, blockSize, config, 2);

        const auto noise = TestUtils::generateNoise(
            (static_cast<double>(totalSamples) + 0.5) / kSampleRate, kSampleRate, 7U);
        juce::AudioBuffer<double> buffer(2, blockSize);

        WorkStats stats;
        std::uint64_t previous = 0;
        std::uint64_t total = 0;
        for (int offset = 0; offset + blockSize <= totalSamples; offset += blockSize) {
            for (int ch = 0; ch < 2; ++ch) {
                std::copy_n(noise.begin() + offset, blockSize, buffer.getWritePointer(ch));
            }
            processor.processBlock(buffer);

            const auto counters = processor.getWorkCountersForTesting();
            const std::uint64_t cumulative = counters.analysisArrivals + counters.synthesisOutputSamples;
            const std::uint64_t work = cumulative - previous;
            previous = cumulative;
            total += work;
            stats.maximum = std::max(stats.maximum, work);
            ++stats.callbacks;
        }
        stats.average = static_cast<double>(total) / static_cast<double>(stats.callbacks);
        return stats;
    }

    void expectEvenWork(const juce::String& label,
                        const dtcwpt::TopologyConfig& config,
                        int blockSize)
    {
        // Several multiples of the alignment unit 2^12 so that every phase of the
        // decimation pattern is covered.
        const auto stats = measureWork(config, blockSize, 4096 * 6);
        const double ratio = static_cast<double>(stats.maximum) / stats.average;

        // For a window of N samples the arrivals of a node at level l are at most
        // ceil(N / 2^l) (the number of multiples of 2^l in the window), and the
        // synthesis ticks follow the same pattern. Summed over the tree, the
        // worst window therefore does less than 2x the average work.
        expect(ratio <= 2.0,
               label + " (block " + juce::String(blockSize) + "): max/average per-callback work is "
                   + juce::String(ratio, 2) + " (max " + juce::String(static_cast<juce::int64>(stats.maximum))
                   + ", avg " + juce::String(stats.average, 1) + "), expected <= 2");
    }

    void testWorkIsEvenlyDistributed()
    {
        expectEvenWork("12-stage DWT", makeConfig(TestUtils::getWaveletTreeDestinations(12), 12), 32);
        expectEvenWork("12-stage DWT", makeConfig(TestUtils::getWaveletTreeDestinations(12), 12), 64);
        expectEvenWork("depth-8 full packet", makeConfig(TestUtils::getFullPacketDestinations(8), 8), 32);
        expectEvenWork("depth-8 full packet", makeConfig(TestUtils::getFullPacketDestinations(8), 8), 64);
    }
#else
    void testWorkIsEvenlyDistributed()
    {
        expect(false, "DTCWPT_ENABLE_TEST_SEAMS must be enabled for work distribution coverage");
    }
#endif
};

static StreamingEngineTests streamingEngineTests;
