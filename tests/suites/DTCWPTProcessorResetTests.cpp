/**
 * @file DTCWPTProcessorResetTests.cpp
 * @brief JUCE UnitTest suite for DTCWPTProcessor::reset().
 *
 * reset() must bring every piece of streaming state back to what
 * prepareToPlay() leaves behind, without allocating, so that a host stop/seek
 * or a loop restart does not leak previously processed audio.
 */

#include <dtcwpt/dtcwpt_processor.h>
#include <dtcwpt/dtcwpt_band_processor.h>
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace {

/**
 * @brief BandProcessor with per-band feedback state and a sidechain dependency.
 *
 * The envelope makes the output depend on all earlier subband samples, so any
 * state the engine fails to clear (or fails to reset in the band processor)
 * shows up as an output difference. The sidechain term does the same for the
 * sidechain analysis path.
 */
class StatefulMixProcessor : public dtcwpt::BandProcessor {
public:
    void prepare(double, int, int numBands, int numChannels) override {
        numBands_ = static_cast<size_t>(numBands);
        envelope_.assign(numBands_ * static_cast<size_t>(numChannels), 0.0);
    }

    void processAllBands(dtcwpt::BandData& data) override {
        for (int ch = 0; ch < data.numChannels; ++ch) {
            for (int b = 0; b < data.numBands; ++b) {
                auto& view = data.bands[static_cast<size_t>(ch)][static_cast<size_t>(b)];
                const dtcwpt::ChannelBandView* sc = data.hasSidechain
                    ? &data.sidechainBands[static_cast<size_t>(ch)][static_cast<size_t>(b)]
                    : nullptr;
                double& env = envelope_[static_cast<size_t>(ch) * numBands_ + static_cast<size_t>(b)];

                for (size_t s = 0; s < view.numSamples; ++s) {
                    env = 0.98 * env + 0.02 * std::hypot(view.re[s], view.im[s]);
                    const double gain = 1.0 / (1.0 + 4.0 * env);
                    const double scRe = (sc != nullptr && s < sc->numSamples) ? sc->re[s] : 0.0;
                    const double scIm = (sc != nullptr && s < sc->numSamples) ? sc->im[s] : 0.0;
                    view.re[s] = view.re[s] * gain + 0.5 * scRe;
                    view.im[s] = view.im[s] * gain + 0.5 * scIm;
                }
            }
        }
    }

    void reset() override {
        std::fill(envelope_.begin(), envelope_.end(), 0.0);
        ++resetCount;
    }

    int resetCount = 0;

private:
    size_t numBands_ = 0;
    std::vector<double> envelope_;
};

using Signal = std::vector<std::vector<double>>;  // [channel][sample]

} // namespace

class DTCWPTProcessorResetTests : public juce::UnitTest {
public:
    DTCWPTProcessorResetTests() : UnitTest("DTCWPTProcessorReset") {}

    void runTest() override {
        beginTest("Without reset, silence still flushes previously processed audio");
        testStaleAudioLeaksWithoutReset();

        beginTest("reset() before prepareToPlay is a harmless no-op");
        testResetBeforePrepare();

        beginTest("reset() turns silent input into exact silence");
        testResetSilencesOutput();

        beginTest("reset() output is bit-identical to a freshly prepared engine");
        testResetMatchesFreshEngine();

        beginTest("reset() clears sidechain state (bit-identical to a fresh engine)");
        testResetClearsSidechainState();

        beginTest("reset() resets the band processor and keeps the latency");
        testResetResetsBandProcessorAndKeepsLatency();

        beginTest("reset() is allocation-free");
        testResetIsAllocationFree();
    }

private:
    static constexpr int kMaxBlock = 512;

    struct Topology {
        const char* name;
        std::vector<std::string> destinations;
        int maxDepth;
    };

    static std::vector<Topology> makeTopologies() {
        return {
            {"wavelet tree depth 6", TestUtils::getWaveletTreeDestinations(6), 8},
            {"full packet depth 3", TestUtils::getFullPacketDestinations(3), 8},
            {"mixed depth", {"H", "LH", "LLH", "LLL"}, 8},
        };
    }

    static dtcwpt::TopologyConfig makeConfig(const Topology& topology) {
        dtcwpt::TopologyConfig config;
        config.destinations = topology.destinations;
        config.maxDepth = topology.maxDepth;
        return config;
    }

    static Signal makeNoise(int numChannels, int numSamples, unsigned int seed) {
        Signal signal;
        for (int ch = 0; ch < numChannels; ++ch) {
            auto noise = TestUtils::generateNoise(
                static_cast<double>(numSamples) / TestUtils::TestConfig::SAMPLE_RATE,
                TestUtils::TestConfig::SAMPLE_RATE,
                seed + static_cast<unsigned int>(ch));
            noise.resize(static_cast<size_t>(numSamples), 0.0);
            signal.push_back(std::move(noise));
        }
        return signal;
    }

    static Signal makeSilence(int numChannels, int numSamples) {
        return Signal(static_cast<size_t>(numChannels),
                      std::vector<double>(static_cast<size_t>(numSamples), 0.0));
    }

    /**
     * Streams `input` through the engine in chunks cycling through `blockPattern`.
     * When `sidechain` is given, each chunk is preceded by processSidechain().
     */
    static Signal run(dtcwpt::DTCWPTProcessor& processor,
                      const Signal& input,
                      const Signal* sidechain,
                      const std::vector<int>& blockPattern) {
        const int numChannels = static_cast<int>(input.size());
        const int totalSamples = static_cast<int>(input[0].size());
        Signal output(static_cast<size_t>(numChannels));

        int cursor = 0;
        size_t patternIndex = 0;
        while (cursor < totalSamples) {
            const int blockSize = std::min(blockPattern[patternIndex % blockPattern.size()],
                                           totalSamples - cursor);
            ++patternIndex;

            juce::AudioBuffer<double> buffer(numChannels, blockSize);
            for (int ch = 0; ch < numChannels; ++ch) {
                std::copy_n(input[static_cast<size_t>(ch)].begin() + cursor, blockSize,
                            buffer.getWritePointer(ch));
            }

            if (sidechain != nullptr) {
                juce::AudioBuffer<double> scBuffer(numChannels, blockSize);
                for (int ch = 0; ch < numChannels; ++ch) {
                    std::copy_n((*sidechain)[static_cast<size_t>(ch)].begin() + cursor, blockSize,
                                scBuffer.getWritePointer(ch));
                }
                processor.processSidechain(scBuffer);
            }

            processor.processBlock(buffer);

            for (int ch = 0; ch < numChannels; ++ch) {
                const double* out = buffer.getReadPointer(ch);
                output[static_cast<size_t>(ch)].insert(output[static_cast<size_t>(ch)].end(),
                                                       out, out + blockSize);
            }
            cursor += blockSize;
        }

        return output;
    }

    static double maxAbs(const Signal& signal) {
        double peak = 0.0;
        for (const auto& channel : signal) {
            for (const double sample : channel) {
                peak = std::max(peak, std::abs(sample));
            }
        }
        return peak;
    }

    void expectBitIdentical(const Signal& expected, const Signal& actual, const juce::String& context) {
        expectEquals(actual.size(), expected.size(), context + ": channel count");
        if (actual.size() != expected.size()) {
            return;
        }

        for (size_t ch = 0; ch < expected.size(); ++ch) {
            expectEquals(actual[ch].size(), expected[ch].size(), context + ": sample count");
            size_t mismatches = 0;
            size_t firstMismatch = 0;
            const size_t n = std::min(actual[ch].size(), expected[ch].size());
            for (size_t i = 0; i < n; ++i) {
                if (actual[ch][i] != expected[ch][i]) {
                    if (mismatches == 0) {
                        firstMismatch = i;
                    }
                    ++mismatches;
                }
            }
            expect(mismatches == 0,
                   context + ": channel " + juce::String(static_cast<int>(ch)) + " has "
                   + juce::String(static_cast<int>(mismatches)) + " mismatching samples (first at "
                   + juce::String(static_cast<int>(firstMismatch)) + ")");
        }
    }

    //==========================================================================

    void testStaleAudioLeaksWithoutReset() {
        // Characterises the problem: the engine holds `latency` samples of past
        // audio, and nothing but prepareToPlay() clears it.
        const Topology topology{"wavelet tree depth 6", TestUtils::getWaveletTreeDestinations(6), 8};
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<StatefulMixProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, kMaxBlock, makeConfig(topology), 1);
        const int latency = processor.getLatency();

        (void) run(processor, makeNoise(1, 16384, 7U), nullptr, {kMaxBlock});

        const auto tail = run(processor, makeSilence(1, latency * 2), nullptr, {kMaxBlock});
        expect(maxAbs(tail) > 0.0,
               "Without reset() or re-prepare, old audio is still emitted when silence arrives");
    }

    void testResetBeforePrepare() {
        dtcwpt::DTCWPTProcessor processor;
        auto bandProcessor = std::make_unique<StatefulMixProcessor>();
        auto* const observed = bandProcessor.get();
        processor.setBandProcessor(std::move(bandProcessor));

        processor.reset();
        processor.reset();
        expectEquals(observed->resetCount, 0, "reset() on an unprepared engine must not touch the band processor");

        const Topology topology{"wavelet tree depth 4", TestUtils::getWaveletTreeDestinations(4), 8};
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, kMaxBlock, makeConfig(topology), 1);

        const auto input = makeNoise(1, 4096, 11U);
        const auto output = run(processor, input, nullptr, {kMaxBlock});
        expect(maxAbs(output) > 0.0, "Engine still works after a no-op reset() before prepare");
    }

    void testResetSilencesOutput() {
        for (const auto& topology : makeTopologies()) {
            for (const int numChannels : {1, 2}) {
                dtcwpt::DTCWPTProcessor processor;
                processor.setBandProcessor(std::make_unique<StatefulMixProcessor>());
                processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, kMaxBlock,
                                        makeConfig(topology), numChannels);
                const int latency = processor.getLatency();
                const juce::String context = juce::String(topology.name) + ", "
                    + juce::String(numChannels) + " ch";

                // An odd sample count with ragged blocks leaves the input FIFO partially
                // filled and the analysis decimators on an odd phase.
                const auto pre = run(processor, makeNoise(numChannels, 12345, 21U), nullptr,
                                     {300, 77, kMaxBlock, 1, 129});
                expect(maxAbs(pre) > 0.0, context + ": noise produced output before reset");

                processor.reset();

                const auto post = run(processor, makeSilence(numChannels, latency * 2 + 1000), nullptr,
                                      {kMaxBlock, 100, 33});
                expect(maxAbs(post) == 0.0,
                       context + ": silence after reset() must give exact zeros, peak = "
                       + juce::String(maxAbs(post)));
            }
        }
    }

    void testResetMatchesFreshEngine() {
        const std::vector<int> pattern{300, 77, kMaxBlock, 1, 129};

        for (const auto& topology : makeTopologies()) {
            for (const int numChannels : {1, 2}) {
                const juce::String context = juce::String(topology.name) + ", "
                    + juce::String(numChannels) + " ch";

                dtcwpt::DTCWPTProcessor resetEngine;
                resetEngine.setBandProcessor(std::make_unique<StatefulMixProcessor>());
                resetEngine.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, kMaxBlock,
                                          makeConfig(topology), numChannels);

                dtcwpt::DTCWPTProcessor freshEngine;
                freshEngine.setBandProcessor(std::make_unique<StatefulMixProcessor>());
                freshEngine.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, kMaxBlock,
                                          makeConfig(topology), numChannels);

                (void) run(resetEngine, makeNoise(numChannels, 12345, 31U), nullptr, pattern);
                resetEngine.reset();

                const auto input = makeNoise(numChannels, 9000, 41U);
                const auto afterReset = run(resetEngine, input, nullptr, pattern);
                const auto fromFresh = run(freshEngine, input, nullptr, pattern);

                expect(maxAbs(fromFresh) > 0.0, context + ": reference output is not silent");
                expectBitIdentical(fromFresh, afterReset, context);

                // A second reset on the same instance keeps the property.
                resetEngine.reset();
                const auto afterSecondReset = run(resetEngine, input, nullptr, pattern);
                expectBitIdentical(fromFresh, afterSecondReset, context + " (second reset)");
            }
        }
    }

    void testResetClearsSidechainState() {
        // Sidechain blocks must divide the internal block so that main and sidechain
        // FIFOs advance together; an odd number of 256-sample blocks leaves both
        // partially filled.
        const std::vector<int> pattern{256};

        for (const auto& topology : makeTopologies()) {
            constexpr int numChannels = 2;
            const juce::String context = juce::String(topology.name) + " with sidechain";

            dtcwpt::DTCWPTProcessor resetEngine;
            resetEngine.setBandProcessor(std::make_unique<StatefulMixProcessor>());
            resetEngine.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, kMaxBlock,
                                      makeConfig(topology), numChannels);

            dtcwpt::DTCWPTProcessor freshEngine;
            freshEngine.setBandProcessor(std::make_unique<StatefulMixProcessor>());
            freshEngine.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, kMaxBlock,
                                      makeConfig(topology), numChannels);

            const auto preMain = makeNoise(numChannels, 256 * 37, 51U);
            const auto preSidechain = makeNoise(numChannels, 256 * 37, 61U);
            (void) run(resetEngine, preMain, &preSidechain, pattern);

            // A sidechain block that never got its processBlock() (e.g. the host stopped).
            juce::AudioBuffer<double> orphan(numChannels, 256);
            for (int ch = 0; ch < numChannels; ++ch) {
                std::fill_n(orphan.getWritePointer(ch), 256, 0.75);
            }
            resetEngine.processSidechain(orphan);

            resetEngine.reset();

            const auto main = makeNoise(numChannels, 256 * 30, 71U);
            const auto sidechain = makeNoise(numChannels, 256 * 30, 81U);
            const auto afterReset = run(resetEngine, main, &sidechain, pattern);
            const auto fromFresh = run(freshEngine, main, &sidechain, pattern);

            expect(maxAbs(fromFresh) > 0.0, context + ": reference output is not silent");
            expectBitIdentical(fromFresh, afterReset, context);

            // Silence on both inputs afterwards must stay exactly silent.
            resetEngine.reset();
            const auto silence = makeSilence(numChannels, resetEngine.getLatency() * 2 + 512);
            const auto silent = run(resetEngine, silence, &silence, pattern);
            expect(maxAbs(silent) == 0.0, context + ": silence after reset() must give exact zeros");
        }
    }

    void testResetResetsBandProcessorAndKeepsLatency() {
        const Topology topology{"wavelet tree depth 6", TestUtils::getWaveletTreeDestinations(6), 8};
        dtcwpt::DTCWPTProcessor processor;
        auto bandProcessor = std::make_unique<StatefulMixProcessor>();
        auto* const observed = bandProcessor.get();
        processor.setBandProcessor(std::move(bandProcessor));
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, kMaxBlock, makeConfig(topology), 2);

        const int latency = processor.getLatency();
        (void) run(processor, makeNoise(2, 5000, 91U), nullptr, {kMaxBlock});

        const int resetsBefore = observed->resetCount;
        processor.reset();
        expectEquals(observed->resetCount, resetsBefore + 1,
                     "reset() should call BandProcessor::reset() exactly once");
        expectEquals(processor.getLatency(), latency, "reset() must not change the latency");
    }

    void testResetIsAllocationFree() {
        const Topology topology{"wavelet tree depth 8", TestUtils::getWaveletTreeDestinations(8), 8};
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor(std::make_unique<StatefulMixProcessor>());
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, kMaxBlock, makeConfig(topology), 2);

        const auto main = makeNoise(2, 256 * 9, 101U);
        const auto sidechain = makeNoise(2, 256 * 9, 111U);
        (void) run(processor, main, &sidechain, {256});

        TestUtils::AllocationCounter counter;
        counter.begin();
        processor.reset();
        processor.reset();
        counter.end();

        expect(counter.getCount() == 0,
               "reset() must not allocate - found " + juce::String(static_cast<int>(counter.getCount())));
    }
};

static DTCWPTProcessorResetTests dtcwptProcessorResetTests;
