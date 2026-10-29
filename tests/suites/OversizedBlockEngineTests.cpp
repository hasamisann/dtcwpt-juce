/**
 * @file OversizedBlockEngineTests.cpp
 * @brief Regression tests for hosts that pass blocks longer than the prepared maximum block size.
 *
 * JUCE only treats maximumExpectedSamplesPerBlock as a hint, so DTCWPTProcessor must accept
 * any block length (including 1 sample and lengths above the prepared maximum) without
 * dropping audio, overflowing internal buffers or allocating on the audio thread.
 */

#include <dtcwpt/dtcwpt_processor.h>
#include <dtcwpt/dtcwpt_band_processor.h>
#include "../util/RegressionFixtures.h"
#include "../util/TestUtils.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kPreparedBlockSize = 256;

/** Records what the engine hands to the band processor, per internal block. */
class CaptureBandProcessor : public dtcwpt::BandProcessor {
public:
    void prepare(double, int, int, int) override {}
    void reset() override {}

    void processAllBands(dtcwpt::BandData& data) override {
        ++calls;
        hasSidechainPerCall.push_back(data.hasSidechain ? 1 : 0);

        const auto count = static_cast<size_t>(data.numChannels * data.numBands);
        mainTrace.resize(count);
        sidechainTrace.resize(count);
        for (int ch = 0; ch < data.numChannels; ++ch) {
            for (int b = 0; b < data.numBands; ++b) {
                const auto index = static_cast<size_t>(ch * data.numBands + b);
                const auto& band = data.bands[static_cast<size_t>(ch)][static_cast<size_t>(b)];
                mainTrace[index].insert(mainTrace[index].end(), band.re, band.re + band.numSamples);
                if (data.hasSidechain) {
                    const auto& sc = data.sidechainBands[static_cast<size_t>(ch)][static_cast<size_t>(b)];
                    for (size_t i = 0; i < sc.numSamples; ++i) {
                        sidechainTrace[index].push_back(sc.re[i]);
                        sidechainTrace[index].push_back(sc.im[i]);
                    }
                }
            }
        }
    }

    int calls = 0;
    std::vector<int> hasSidechainPerCall;
    std::vector<std::vector<double>> mainTrace;
    std::vector<std::vector<double>> sidechainTrace;
};

/** Leaves every band untouched (the capture processor above allocates when recording). */
class PassthroughBandProcessor : public dtcwpt::BandProcessor {
public:
    void prepare(double, int, int, int) override {}
    void reset() override {}
};

dtcwpt::TopologyConfig makeDwtConfig(int levels, int maxDepth = 8) {
    dtcwpt::TopologyConfig config;
    config.destinations = TestUtils::getWaveletTreeDestinations(levels);
    config.maxDepth = maxDepth;
    return config;
}

} // namespace

class OversizedBlockEngineTests : public juce::UnitTest {
public:
    OversizedBlockEngineTests()
        : juce::UnitTest("OversizedBlockEngineTests")
    {
    }

    void runTest() override
    {
        beginTest("Prepared-size blocks reconstruct perfectly (control)");
        testPreparedSizeControl();

        beginTest("Blocks longer than the prepared maximum reconstruct perfectly");
        testOversizedBlocksReconstruct();

        beginTest("Single-sample and irregular block lengths reconstruct perfectly");
        testTinyAndIrregularBlocksReconstruct();

        beginTest("Output does not depend on how the host partitions the stream");
        testOutputIsPartitionInvariant();

        beginTest("Oversized blocks do not allocate");
        testOversizedBlocksDoNotAllocate();

        beginTest("Oversized sidechain stays aligned with the main signal");
        testOversizedSidechainStaysAligned();

        beginTest("processSidechain rejects oversized blocks without overflowing");
        testProcessSidechainRejectsOversizedBlocks();
    }

private:
    struct RunResult {
        std::vector<double> output;
        int latency = 0;
    };

    static std::vector<double> makeSine(int numSamples) {
        std::vector<double> signal(static_cast<size_t>(numSamples));
        for (int i = 0; i < numSamples; ++i) {
            signal[static_cast<size_t>(i)] =
                0.5 * std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / kSampleRate);
        }
        return signal;
    }

    /** Feeds `input` followed by `latency` zeros through the engine using the cyclic block-length `pattern`. */
    static RunResult runMono(const std::vector<double>& input, const std::vector<int>& pattern) {
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<CaptureBandProcessor>());
        processor.prepareToPlay(kSampleRate, kPreparedBlockSize, makeDwtConfig(6), 1);

        RunResult result;
        result.latency = processor.getLatency();

        std::vector<double> stream = input;
        stream.resize(input.size() + static_cast<size_t>(result.latency), 0.0);

        size_t cursor = 0;
        size_t patternIndex = 0;
        while (cursor < stream.size()) {
            const int requested = pattern[patternIndex++ % pattern.size()];
            const int length = static_cast<int>(std::min<size_t>(static_cast<size_t>(requested), stream.size() - cursor));

            juce::AudioBuffer<double> buffer(1, length);
            std::copy_n(stream.begin() + static_cast<std::ptrdiff_t>(cursor), length, buffer.getWritePointer(0));
            processor.processBlock(buffer);
            result.output.insert(result.output.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + length);

            cursor += static_cast<size_t>(length);
        }

        return result;
    }

    void expectPerfectReconstruction(const std::vector<int>& pattern, const juce::String& label) {
        const auto input = makeSine(8192);
        const auto result = runMono(input, pattern);
        const auto check = RegressionFixtures::evaluateReconstruction(input, result.output, result.latency);

        expect(check.passesMseThreshold,
               label + " should reconstruct at <= -200 dB, got " + juce::String(check.mseDb, 3)
                   + " dB with max abs error " + juce::String(check.maxAbsError, 12));
    }

    void testPreparedSizeControl() {
        expectPerfectReconstruction({kPreparedBlockSize}, "Blocks of the prepared size");
        expectPerfectReconstruction({100}, "Blocks smaller than the prepared size");
    }

    void testOversizedBlocksReconstruct() {
        expectPerfectReconstruction({1024}, "1024-sample blocks (prepared 256)");
        expectPerfectReconstruction({300}, "300-sample blocks (prepared 256)");
        expectPerfectReconstruction({257}, "257-sample blocks (prepared 256)");
        expectPerfectReconstruction({8192}, "One block spanning the whole signal");
    }

    void testTinyAndIrregularBlocksReconstruct() {
        expectPerfectReconstruction({1}, "1-sample blocks");
        expectPerfectReconstruction({1, 7, 255, 256, 257, 513, 1024, 3, 1000, 2},
                                    "Irregular block lengths mixing small and oversized blocks");
    }

    void testOutputIsPartitionInvariant() {
        const auto input = makeSine(8192);
        const auto reference = runMono(input, {kPreparedBlockSize});

        const std::vector<std::vector<int>> patterns = {
            {1024}, {300}, {1}, {1, 7, 255, 256, 257, 513, 1024, 3, 1000, 2}};

        for (const auto& pattern : patterns) {
            const auto candidate = runMono(input, pattern);
            expect(candidate.output.size() == reference.output.size(),
                   "Output length should not depend on the block partition");
            expect(candidate.output == reference.output,
                   "Output should be bit-identical to the prepared-size run for a pattern of "
                       + juce::String(static_cast<int>(pattern.size())) + " block length(s) starting with "
                       + juce::String(pattern.front()));
        }
    }

    void testOversizedBlocksDoNotAllocate() {
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<PassthroughBandProcessor>());
        processor.prepareToPlay(kSampleRate, kPreparedBlockSize, makeDwtConfig(6), 2);

        juce::AudioBuffer<double> oversized(2, 1024);
        juce::AudioBuffer<double> sidechain(2, 1024);
        oversized.clear();
        sidechain.clear();

        TestUtils::AllocationCounter counter;
        counter.begin();
        for (int iteration = 0; iteration < 4; ++iteration) {
            processor.processBlock(oversized);
            processor.processBlock(oversized, &sidechain);
        }
        counter.end();

        expect(counter.getCount() == 0,
               "Oversized processBlock calls should not allocate; found "
                   + juce::String(static_cast<int>(counter.getCount())) + " allocations");
    }

    /** Runs the engine with a noise sidechain and returns what the band processor observed. */
    static std::unique_ptr<CaptureBandProcessor> runWithSidechain(const std::vector<int>& pattern) {
        dtcwpt::DTCWPTProcessor processor;
        auto capture = std::make_unique<CaptureBandProcessor>();
        auto* observed = capture.get();
        processor.setBandProcessor(std::move(capture));
        processor.prepareToPlay(kSampleRate, kPreparedBlockSize, makeDwtConfig(6), 1);

        const auto mainSignal = makeSine(4096);
        const auto sidechainSignal = TestUtils::generateNoise(4096.0 / kSampleRate, kSampleRate);

        size_t cursor = 0;
        size_t patternIndex = 0;
        while (cursor < mainSignal.size()) {
            const int requested = pattern[patternIndex++ % pattern.size()];
            const int length = static_cast<int>(std::min<size_t>(static_cast<size_t>(requested), mainSignal.size() - cursor));

            juce::AudioBuffer<double> mainBuffer(1, length);
            juce::AudioBuffer<double> scBuffer(1, length);
            std::copy_n(mainSignal.begin() + static_cast<std::ptrdiff_t>(cursor), length, mainBuffer.getWritePointer(0));
            std::copy_n(sidechainSignal.begin() + static_cast<std::ptrdiff_t>(cursor), length, scBuffer.getWritePointer(0));
            processor.processBlock(mainBuffer, &scBuffer);

            cursor += static_cast<size_t>(length);
        }

        // The observer is owned by the processor; copy the data out before it goes away.
        auto copy = std::make_unique<CaptureBandProcessor>();
        copy->calls = observed->calls;
        copy->hasSidechainPerCall = observed->hasSidechainPerCall;
        copy->mainTrace = observed->mainTrace;
        copy->sidechainTrace = observed->sidechainTrace;
        return copy;
    }

    void testOversizedSidechainStaysAligned() {
        const auto reference = runWithSidechain({kPreparedBlockSize});

        expect(reference->calls == 4096 / kPreparedBlockSize,
               "Reference run should process one internal block per 256 samples");
        expect(std::all_of(reference->hasSidechainPerCall.begin(), reference->hasSidechainPerCall.end(),
                           [](int flag) { return flag == 1; }),
               "Reference run should see the sidechain in every internal block");

        const std::vector<std::vector<int>> patterns = {{1024}, {300}, {1}, {1, 7, 255, 256, 257, 513, 1024, 3, 1000, 2}};
        for (const auto& pattern : patterns) {
            const auto candidate = runWithSidechain(pattern);
            const juce::String label = "pattern starting with " + juce::String(pattern.front());

            expect(!candidate->hasSidechainPerCall.empty()
                       && std::all_of(candidate->hasSidechainPerCall.begin(), candidate->hasSidechainPerCall.end(),
                                      [](int flag) { return flag == 1; }),
                   "Every streaming callback must have aligned sidechain data for " + label);
            expect(candidate->mainTrace == reference->mainTrace,
                   "Main subbands should match the reference for " + label);
            expect(candidate->sidechainTrace == reference->sidechainTrace,
                   "Sidechain subbands should stay aligned with the main signal for " + label);
        }
    }

    void testProcessSidechainRejectsOversizedBlocks() {
        dtcwpt::DTCWPTProcessor processor;
        auto capture = std::make_unique<CaptureBandProcessor>();
        auto* observed = capture.get();
        processor.setBandProcessor(std::move(capture));
        processor.prepareToPlay(kSampleRate, kPreparedBlockSize, makeDwtConfig(6), 1);

        const auto input = makeSine(1024);
        juce::AudioBuffer<double> mainBuffer(1, 1024);
        juce::AudioBuffer<double> sidechainBuffer(1, 1024);
        std::copy(input.begin(), input.end(), mainBuffer.getWritePointer(0));
        sidechainBuffer.clear();

        // Longer than the prepared maximum: the sidechain is dropped for this block instead of overflowing.
        // (Debug builds report this through jassert; the check below covers release builds.)
        processor.processSidechain(sidechainBuffer);
        processor.processBlock(mainBuffer);

        expect(observed->calls == 1024 / kPreparedBlockSize,
               "All four internal blocks should be processed for a 1024-sample block");
        expect(std::none_of(observed->hasSidechainPerCall.begin(), observed->hasSidechainPerCall.end(),
                            [](int flag) { return flag == 1; }),
               "An oversized sidechain should be disabled for the block");
    }
};

static OversizedBlockEngineTests oversizedBlockEngineTests;
