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
#include <cmath>
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

// Records how the engine drives the BandProcessor.
class RecordingBandProcessor : public dtcwpt::BandProcessor {
public:
    void prepare(double, int maxSamples, int bands, int) override
    {
        preparedMaxSamplesPerBand = maxSamples;
        maxSamplesSeen.assign(static_cast<size_t>(bands), 0);
    }

    void reset() override {}

    void processAllBands(dtcwpt::BandData& data) override
    {
        ++calls;
        bool any = false;
        for (int b = 0; b < data.numBands; ++b) {
            const size_t n = data.bands[0][static_cast<size_t>(b)].numSamples;
            any = any || n > 0;
            maxSamplesSeen[static_cast<size_t>(b)] = std::max(maxSamplesSeen[static_cast<size_t>(b)], n);

            for (int ch = 0; ch < data.numChannels; ++ch) {
                if (data.bands[static_cast<size_t>(ch)][static_cast<size_t>(b)].numSamples != n) {
                    channelCountMismatch = true;
                }
                if (data.hasSidechain
                    && data.sidechainBands[static_cast<size_t>(ch)][static_cast<size_t>(b)].numSamples != n) {
                    sidechainCountMismatch = true;
                }
            }
        }
        if (! any) {
            emptyCalls++;
        }
        if (data.hasSidechain) {
            ++sidechainCalls;
        }
    }

    int preparedMaxSamplesPerBand = 0;
    std::vector<size_t> maxSamplesSeen;
    int calls = 0;
    int emptyCalls = 0;
    int sidechainCalls = 0;
    bool channelCountMismatch = false;
    bool sidechainCountMismatch = false;
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

        beginTest("Latency equals the filter-path delay");
        testLatencyValues();

        beginTest("Impulse is delayed by exactly getLatency() for any host block pattern");
        testImpulseDelayEqualsLatency();

        beginTest("Blocks longer than maxBlockSize are processed in chunks");
        testOversizedBlocks();

        beginTest("BandProcessor receives at most maxSamplesPerBand samples per band");
        testBandSampleCounts();

        beginTest("BandProcessor is only called when subband samples are available");
        testNoEmptyBandProcessorCalls();

        beginTest("Sidechain band counts match the main bands, also after a gap");
        testSidechainCountsAfterGap();

        beginTest("Oversized sidechain block is ignored safely");
        testOversizedSidechainIgnored();

        beginTest("Processing stays allocation-free for small, odd and oversized blocks");
        testAllocationFree();

        beginTest("Re-prepare restarts the stream");
        testReprepareRestartsStream();
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

    void testLatencyValues()
    {
        // Filter-path delays of the topologies (the former reported latency minus the internal block size).
        expectEquals(latencyFor(makeConfig(TestUtils::getWaveletTreeDestinations(6), 6), 512), 814,
                     "6-stage DWT latency should be its filter-path delay");
        expectEquals(latencyFor(makeConfig(TestUtils::getWaveletTreeDestinations(12), 12), 512), 53230,
                     "12-stage DWT latency should be its filter-path delay");
        expectEquals(latencyFor(makeConfig(TestUtils::getFullPacketDestinations(8), 8), 512), 3310,
                     "Depth-8 full packet latency should be its filter-path delay");
    }

    // Host block patterns used to drive the engine with a given maximum block size.
    static std::vector<std::vector<int>> makeBlockPatterns()
    {
        return {
            {1},
            {32},
            {64},
            {1000},
            {1, 7, 100, 33, 1000, 2, 511, 64, 3},
        };
    }

    // Feeds a unit impulse at `position`, collects the output over the given block pattern and
    // returns the output samples.
    static std::vector<double> runImpulse(dtcwpt::DTCWPTProcessor& processor,
                                          int totalSamples,
                                          int position,
                                          const std::vector<int>& pattern)
    {
        std::vector<double> output;
        output.reserve(static_cast<size_t>(totalSamples));

        int cursor = 0;
        size_t patternIndex = 0;
        while (cursor < totalSamples) {
            const int n = std::min(pattern[patternIndex % pattern.size()], totalSamples - cursor);
            ++patternIndex;

            juce::AudioBuffer<double> buffer(1, n);
            buffer.clear();
            if (position >= cursor && position < cursor + n) {
                buffer.setSample(0, position - cursor, 1.0);
            }
            processor.processBlock(buffer);
            output.insert(output.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + n);
            cursor += n;
        }
        return output;
    }

    void expectDelayedImpulse(const std::vector<double>& output, int position, int latency, const juce::String& label)
    {
        const int expectedIndex = position + latency;
        double worstOther = 0.0;
        for (int i = 0; i < static_cast<int>(output.size()); ++i) {
            if (i != expectedIndex) {
                worstOther = std::max(worstOther, std::abs(output[static_cast<size_t>(i)]));
            }
        }
        expect(expectedIndex < static_cast<int>(output.size()), label + ": test signal too short");
        expectWithinAbsoluteError(output[static_cast<size_t>(expectedIndex)], 1.0, 1.0e-9,
                                  label + ": impulse should appear exactly at index " + juce::String(expectedIndex));
        expect(worstOther < 1.0e-9,
               label + ": output away from the delayed impulse should be silent, largest " + juce::String(worstOther));
    }

    void testImpulseDelayEqualsLatency()
    {
        const std::vector<NamedTopology> topologies = {
            {"6-stage DWT", makeConfig(TestUtils::getWaveletTreeDestinations(6), 6)},
            {"depth-5 full packet", makeConfig(TestUtils::getFullPacketDestinations(5), 5)},
            {"random depth-7", makeConfig(TestUtils::getRandomPacketDestinations(7, 11), 7)},
            {"mixed {H, LL, LH}", makeConfig({"H", "LL", "LH"}, 2)},
        };

        for (const auto& topology : topologies) {
            for (const auto& pattern : makeBlockPatterns()) {
                dtcwpt::DTCWPTProcessor processor;
                processor.setBandProcessor(std::make_unique<PassthroughBandProcessor>());
                processor.prepareToPlay(kSampleRate, 1000, topology.config, 1);
                const int latency = processor.getLatency();

                const int position = 37;
                const auto output = runImpulse(processor, position + latency + 64, position, pattern);
                expectDelayedImpulse(output, position, latency,
                                     juce::String(topology.name) + ", first block size " + juce::String(pattern.front()));
            }
        }
    }

    // Runs `input` through a freshly prepared engine, with the given host block lengths.
    static std::vector<double> runBlocks(const dtcwpt::TopologyConfig& config,
                                         int maxBlockSize,
                                         const std::vector<double>& input,
                                         const std::vector<int>& blockLengths)
    {
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughBandProcessor>());
        processor.prepareToPlay(kSampleRate, maxBlockSize, config, 1);

        std::vector<double> output;
        size_t cursor = 0;
        for (const int length : blockLengths) {
            juce::AudioBuffer<double> buffer(1, length);
            std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(cursor), length, buffer.getWritePointer(0));
            processor.processBlock(buffer);
            output.insert(output.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + length);
            cursor += static_cast<size_t>(length);
        }
        return output;
    }

    void testOversizedBlocks()
    {
        const auto config = makeConfig(TestUtils::getFullPacketDestinations(5), 5);
        const auto input = TestUtils::generateNoise(3000.5 / kSampleRate, kSampleRate, 5U);
        const std::vector<int> oversized = {1000, 300, 1700};

        std::vector<int> reference;
        int remaining = 3000;
        while (remaining > 0) {
            reference.push_back(std::min(64, remaining));
            remaining -= reference.back();
        }

        const auto oversizedOutput = runBlocks(config, 64, input, oversized);
        const auto referenceOutput = runBlocks(config, 64, input, reference);

        expectEquals(static_cast<int>(oversizedOutput.size()), static_cast<int>(referenceOutput.size()),
                     "Both runs should produce the same number of samples");
        expect(oversizedOutput == referenceOutput, "Processing blocks longer than maxBlockSize should equal processing them in chunks");

        // Oversized blocks must also reconstruct the signal.
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughBandProcessor>());
        processor.prepareToPlay(kSampleRate, 64, config, 1);
        const int latency = processor.getLatency();
        const auto longInput = TestUtils::generateNoise(0.5, kSampleRate, 9U);
        std::vector<double> padded = longInput;
        padded.resize(longInput.size() + static_cast<size_t>(latency), 0.0);
        const auto output = runBlocks(config, 64, padded, {static_cast<int>(padded.size())});
        double worst = 0.0;
        for (size_t i = 0; i < longInput.size(); ++i) {
            worst = std::max(worst, std::abs(output[i + static_cast<size_t>(latency)] - longInput[i]));
        }
        expect(worst < 1.0e-9, "A single oversized block should reconstruct, worst error " + juce::String(worst));
    }

    void testBandSampleCounts()
    {
        // {H, LL, LH}: H is a level-1 band, LL and LH are level-2 bands.
        const std::vector<size_t> expectedBound = {4, 2, 2};  // ceil(8 / 2), ceil(8 / 4), ceil(8 / 4)

        for (const auto& pattern : std::vector<std::vector<int>>{{8}, {5, 3}, {1, 2, 3, 8, 7}, {6}}) {
            dtcwpt::DTCWPTProcessor processor;
            auto recorder = std::make_unique<RecordingBandProcessor>();
            auto* recorderPtr = recorder.get();
            processor.setBandProcessor(std::move(recorder));
            processor.prepareToPlay(kSampleRate, 8, makeConfig({"H", "LL", "LH"}, 2), 2);

            expectEquals(recorderPtr->preparedMaxSamplesPerBand, 4,
                         "BandProcessor::prepare should get ceil(maxBlockSize / 2^minLevel) samples per band");

            juce::AudioBuffer<double> buffer(2, 8);
            for (int round = 0; round < 40; ++round) {
                for (const int n : pattern) {
                    buffer.setSize(2, n, false, false, true);
                    buffer.clear();
                    processor.processBlock(buffer);
                }
            }

            for (size_t band = 0; band < expectedBound.size(); ++band) {
                expect(recorderPtr->maxSamplesSeen[band] <= expectedBound[band],
                       "Band " + juce::String(static_cast<int>(band)) + " exceeded its per-call capacity");
            }
            expect(! recorderPtr->channelCountMismatch, "Channels should receive the same number of samples per band");
        }

        // With whole-block calls the bound is reached exactly (it is tight, not just an upper limit).
        dtcwpt::DTCWPTProcessor processor;
        auto recorder = std::make_unique<RecordingBandProcessor>();
        auto* recorderPtr = recorder.get();
        processor.setBandProcessor(std::move(recorder));
        processor.prepareToPlay(kSampleRate, 8, makeConfig({"H", "LL", "LH"}, 2), 1);
        juce::AudioBuffer<double> buffer(1, 8);
        for (int i = 0; i < 4; ++i) {
            buffer.clear();
            processor.processBlock(buffer);
        }
        expect(recorderPtr->maxSamplesSeen == expectedBound,
               "Aligned blocks of maxBlockSize should fill the per-band capacity exactly");
    }

    void testNoEmptyBandProcessorCalls()
    {
        // Eight level-3 bands: with single-sample blocks a band sample appears every 8th sample only.
        dtcwpt::DTCWPTProcessor processor;
        auto recorder = std::make_unique<RecordingBandProcessor>();
        auto* recorderPtr = recorder.get();
        processor.setBandProcessor(std::move(recorder));
        processor.prepareToPlay(kSampleRate, 4, makeConfig(TestUtils::getFullPacketDestinations(3), 3), 1);

        juce::AudioBuffer<double> buffer(1, 1);
        for (int i = 0; i < 64; ++i) {
            buffer.clear();
            processor.processBlock(buffer);
        }
        expectEquals(recorderPtr->calls, 8, "Only every 8th single-sample block completes a level-3 band sample");
        expectEquals(recorderPtr->emptyCalls, 0, "BandProcessor must not be called without subband samples");
    }

    void testSidechainCountsAfterGap()
    {
        const auto config = makeConfig(TestUtils::getFullPacketDestinations(4), 4);
        dtcwpt::DTCWPTProcessor processor;
        auto recorder = std::make_unique<RecordingBandProcessor>();
        auto* recorderPtr = recorder.get();
        processor.setBandProcessor(std::move(recorder));
        processor.prepareToPlay(kSampleRate, 64, config, 2);

        const auto noise = TestUtils::generateNoise(0.1, kSampleRate, 3U);
        size_t cursor = 0;
        auto runBlock = [&](int n, bool withSidechain) {
            juce::AudioBuffer<double> main(2, n);
            juce::AudioBuffer<double> side(1, n);
            for (int i = 0; i < n; ++i) {
                main.setSample(0, i, noise[cursor + static_cast<size_t>(i)]);
                main.setSample(1, i, noise[cursor + static_cast<size_t>(i)]);
                side.setSample(0, i, noise[cursor + static_cast<size_t>(i) + 1000]);
            }
            if (withSidechain) {
                processor.processSidechain(side);
            }
            processor.processBlock(main);
            cursor += static_cast<size_t>(n);
        };

        // Sidechain on aligned blocks, then gaps of 13 and 7 samples (not multiples of 2^4), then sidechain again.
        runBlock(32, true);
        runBlock(13, false);
        const int callsBeforeResume = recorderPtr->sidechainCalls;
        runBlock(3, true);
        runBlock(21, true);
        runBlock(64, true);
        runBlock(5, true);
        runBlock(30, true);
        runBlock(7, false);
        runBlock(2, true);
        runBlock(9, true);
        runBlock(50, true);

        expect(recorderPtr->sidechainCalls > callsBeforeResume, "Sidechain should be active after the gap");
        expect(! recorderPtr->sidechainCountMismatch,
               "Sidechain bands should hold as many samples as the main bands, also after a gap");
        expect(! recorderPtr->channelCountMismatch, "Channels should receive the same number of samples per band");
    }

    void testOversizedSidechainIgnored()
    {
        dtcwpt::DTCWPTProcessor processor;
        auto recorder = std::make_unique<RecordingBandProcessor>();
        auto* recorderPtr = recorder.get();
        processor.setBandProcessor(std::move(recorder));
        processor.prepareToPlay(kSampleRate, 64, makeConfig(TestUtils::getFullPacketDestinations(3), 3), 1);

        juce::AudioBuffer<double> main(1, 64);
        juce::AudioBuffer<double> longSidechain(1, 200);
        main.clear();
        longSidechain.clear();
        processor.processSidechain(longSidechain);
        processor.processBlock(main);

        expectEquals(recorderPtr->sidechainCalls, 0, "A sidechain block longer than maxBlockSize should be ignored");
        expect(recorderPtr->calls > 0, "The main signal should still be processed");
    }

    void testAllocationFree()
    {
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughBandProcessor>());
        processor.prepareToPlay(kSampleRate, 64, makeConfig(TestUtils::getFullPacketDestinations(6), 6), 2);

        juce::AudioBuffer<double> oneSample(2, 1);
        juce::AudioBuffer<double> odd(2, 37);
        juce::AudioBuffer<double> full(2, 64);
        juce::AudioBuffer<double> oversized(2, 300);
        juce::AudioBuffer<double> sidechain(2, 37);
        oneSample.clear();
        odd.clear();
        full.clear();
        oversized.clear();
        sidechain.clear();

        TestUtils::AllocationCounter counter;
        counter.begin();
        for (int i = 0; i < 20; ++i) {
            processor.processBlock(oneSample);
            processor.processBlock(odd);
            processor.processBlock(full);
            processor.processBlock(oversized);
            processor.processSidechain(sidechain);
            processor.processBlock(odd);
        }
        counter.end();

        expectEquals(static_cast<int>(counter.getCount()), 0,
                     "processBlock()/processSidechain() should not allocate for any block length");
    }

    void testReprepareRestartsStream()
    {
        const auto config = makeConfig(TestUtils::getFullPacketDestinations(5), 5);
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughBandProcessor>());
        processor.prepareToPlay(kSampleRate, 100, config, 1);

        juce::AudioBuffer<double> noise(1, 77);
        for (int i = 0; i < 77; ++i) {
            noise.setSample(0, i, 0.25 * std::sin(0.3 * i));
        }
        processor.processBlock(noise);  // leaves the stream at an unaligned position

        processor.prepareToPlay(kSampleRate, 100, config, 1);
        const int latency = processor.getLatency();
        const auto output = runImpulse(processor, 5 + latency + 64, 5, {32, 17, 100});
        expectDelayedImpulse(output, 5, latency, "after re-prepare");
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
