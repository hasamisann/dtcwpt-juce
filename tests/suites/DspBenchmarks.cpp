/**
 * @file DspBenchmarks.cpp
 * @brief Processing-time benchmarks for the DSP hot paths (issue #18).
 *
 * Registered under the "Benchmark" category: a plain run of dtcwpt_tests skips it
 * (see main.cpp). Run it explicitly, preferably in a Release build:
 *
 *     dtcwpt_tests --test DspBenchmarks
 *
 * Setup: 48 kHz, stereo, 512-sample blocks, 10 s of noise per repetition.
 * For every case the suite reports the mean and maximum time per block, and the
 * mean as a share of the real-time budget of one block (the "load" figure),
 * taking the median of the repetitions' means to damp scheduler noise.
 * Timing assertions are deliberately absent: the numbers are for comparison
 * between revisions on the same machine, not pass/fail thresholds.
 */

#include <dtcwpt/dtcwpt_processor.h>
#include "MorphBandProcessor.h"
#include "../util/TestUtils.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr int kChannels = 2;
constexpr int kRepetitions = 5;
constexpr int kBlocksPerRepetition = static_cast<int>(10.0 * kSampleRate / kBlockSize);  // ~10 s
constexpr int kWarmupBlocks = 100;

struct Stats {
    double meanUs = 0.0;
    double maxUs = 0.0;
};

/// Reduces per-repetition stats: median of the means, overall maximum.
Stats summarise(std::vector<Stats> reps) {
    std::sort(reps.begin(), reps.end(), [](const Stats& a, const Stats& b) { return a.meanUs < b.meanUs; });
    Stats out;
    out.meanUs = reps[reps.size() / 2].meanUs;
    for (const auto& r : reps) out.maxUs = std::max(out.maxUs, r.maxUs);
    return out;
}

} // namespace

class DspBenchmarks : public juce::UnitTest {
public:
    DspBenchmarks() : UnitTest("DspBenchmarks", "Benchmark") {}

    void runTest() override {
#if defined(NDEBUG)
        const bool optimised = true;
#else
        const bool optimised = false;
#endif
        report(juce::String("Build: ") + (optimised ? "optimised (NDEBUG)" : "NOT optimised - numbers are not representative"));
        report("48 kHz, stereo, " + juce::String(kBlockSize) + "-sample blocks, "
               + juce::String(kBlocksPerRepetition) + " blocks x " + juce::String(kRepetitions)
               + " repetitions (median of means, overall max)");

        beginTest("DTCWPTProcessor, 6-level DWT");
        benchmarkProcessor("DTCWPTProcessor 6-level DWT", TestUtils::getWaveletTreeDestinations(6), 6);

        beginTest("DTCWPTProcessor, depth-8 full packet");
        benchmarkProcessor("DTCWPTProcessor depth-8 full packet", TestUtils::getFullPacketDestinations(8), 8);

        beginTest("MorphBandProcessor, 6-level DWT band layout");
        benchmarkMorph("MorphBandProcessor 6-level DWT layout", dwtBandSizes(6));

        beginTest("MorphBandProcessor, depth-8 full packet band layout");
        benchmarkMorph("MorphBandProcessor depth-8 full packet layout", std::vector<std::size_t>(256, kBlockSize / 256));
    }

private:
    void report(const juce::String& line) {
        logMessage(line);
    }

    void reportStats(const juce::String& label, const Stats& s) {
        const double blockBudgetUs = 1.0e6 * kBlockSize / kSampleRate;
        report(label + ": mean " + juce::String(s.meanUs, 1) + " us/block, max " + juce::String(s.maxUs, 1)
               + " us/block, load " + juce::String(100.0 * s.meanUs / blockBudgetUs, 2) + " % of one core");
        expect(s.meanUs > 0.0, label + ": a measurement was taken");
    }

    /// Per-block sample counts of the bands of an N-level DWT (low path split repeatedly).
    static std::vector<std::size_t> dwtBandSizes(int levels) {
        std::vector<std::size_t> sizes;
        std::size_t n = kBlockSize;
        for (int l = 0; l < levels; ++l) {
            n /= 2;
            sizes.push_back(n);
        }
        sizes.push_back(n);
        return sizes;
    }

    void benchmarkProcessor(const juce::String& label, const std::vector<std::string>& destinations, int maxDepth) {
        std::vector<double> noise = TestUtils::generateNoise(
            static_cast<double>(kBlocksPerRepetition) * kBlockSize / kSampleRate, kSampleRate);
        std::vector<Stats> reps;

        for (int rep = 0; rep < kRepetitions; ++rep) {
            dtcwpt::DTCWPTProcessor processor;
            dtcwpt::TopologyConfig config;
            config.destinations = destinations;
            config.maxDepth = maxDepth;
            processor.prepareToPlay(kSampleRate, kBlockSize, config, kChannels);

            juce::AudioBuffer<double> buffer(kChannels, kBlockSize);
            Stats stats;
            double totalUs = 0.0;
            int timed = 0;
            const int totalBlocks = kWarmupBlocks + kBlocksPerRepetition;
            for (int blk = 0; blk < totalBlocks; ++blk) {
                for (int ch = 0; ch < kChannels; ++ch) {
                    for (int i = 0; i < kBlockSize; ++i) {
                        const std::size_t idx = (static_cast<std::size_t>(blk) * kBlockSize + static_cast<std::size_t>(i)
                                                 + static_cast<std::size_t>(ch) * 977) % noise.size();
                        buffer.setSample(ch, i, noise[idx]);
                    }
                }
                const auto t0 = std::chrono::steady_clock::now();
                processor.processBlock(buffer);
                const auto t1 = std::chrono::steady_clock::now();
                if (blk < kWarmupBlocks) continue;
                const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                totalUs += us;
                ++timed;
                stats.maxUs = std::max(stats.maxUs, us);
            }
            stats.meanUs = totalUs / timed;
            reps.push_back(stats);
        }
        reportStats(label, summarise(reps));
    }

    void benchmarkMorph(const juce::String& label, const std::vector<std::size_t>& bandSizes) {
        const int numBands = static_cast<int>(bandSizes.size());
        std::mt19937 rng(TestUtils::TestConfig::RANDOM_SEED);
        std::uniform_real_distribution<double> value(-0.5, 0.5);

        // Pristine inputs, copied into the working buffers before every timed call
        // (processAllBands writes its result in place).
        using Plane = std::vector<std::vector<std::vector<double>>>;  // [ch][band][sample]
        auto makePlane = [&](bool randomise) {
            Plane p(kChannels);
            for (auto& ch : p) {
                ch.resize(bandSizes.size());
                for (std::size_t b = 0; b < bandSizes.size(); ++b) {
                    ch[b].resize(bandSizes[b]);
                    if (randomise) for (auto& v : ch[b]) v = value(rng);
                }
            }
            return p;
        };
        const Plane srcMainRe = makePlane(true), srcMainIm = makePlane(true);
        const Plane srcScRe = makePlane(true), srcScIm = makePlane(true);
        Plane mainRe = srcMainRe, mainIm = srcMainIm, scRe = srcScRe, scIm = srcScIm;

        std::vector<int> ids(bandSizes.size());
        for (std::size_t b = 0; b < ids.size(); ++b) ids[b] = static_cast<int>(b) + 1;

        dtcwpt::BandData data;
        data.numChannels = kChannels;
        data.numBands = numBands;
        data.hasSidechain = true;
        data.destinationIds = &ids;
        data.bands.resize(kChannels);
        data.sidechainBands.resize(kChannels);
        for (int ch = 0; ch < kChannels; ++ch) {
            const auto c = static_cast<std::size_t>(ch);
            for (std::size_t b = 0; b < bandSizes.size(); ++b) {
                data.bands[c].push_back({mainRe[c][b].data(), mainIm[c][b].data(), bandSizes[b]});
                data.sidechainBands[c].push_back({scRe[c][b].data(), scIm[c][b].data(), bandSizes[b]});
            }
        }

        std::size_t maxBand = 0;
        for (auto s : bandSizes) maxBand = std::max(maxBand, s);

        std::vector<Stats> reps;
        for (int rep = 0; rep < kRepetitions; ++rep) {
            MorphBandProcessor proc;
            proc.prepare(kSampleRate, static_cast<int>(maxBand), numBands, kChannels);
            // Mid-range, non-trivial settings so that every branch of the morph is exercised.
            proc.setTargets(0.5, 0.6, 0.05, false, false);
            proc.reset();

            Stats stats;
            double totalUs = 0.0;
            int timed = 0;
            const int totalBlocks = kWarmupBlocks + kBlocksPerRepetition;
            for (int blk = 0; blk < totalBlocks; ++blk) {
                mainRe = srcMainRe;
                mainIm = srcMainIm;
                const auto t0 = std::chrono::steady_clock::now();
                proc.processAllBands(data);
                const auto t1 = std::chrono::steady_clock::now();
                if (blk < kWarmupBlocks) continue;
                const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                totalUs += us;
                ++timed;
                stats.maxUs = std::max(stats.maxUs, us);
            }
            stats.meanUs = totalUs / timed;
            reps.push_back(stats);
        }
        reportStats(label, summarise(reps));
    }
};

static DspBenchmarks dspBenchmarks;
