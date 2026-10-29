/**
 * @file FrequencyOrderTests.cpp
 * @brief Regression tests for the path-string <-> frequency correspondence (issue #12).
 *
 * After a high-pass + 2:1 decimation the sub-band spectrum is mirrored, so the
 * leaves of a wavelet packet tree are NOT in natural (L = lower half,
 * H = upper half) order. These tests feed sine waves through the real engine
 * and check which band actually carries the energy.
 */

#include <dtcwpt/dtcwpt_processor.h>
#include <dtcwpt/dtcwpt_band_processor.h>
#include <dtcwpt/dtcwpt_frequency_mapping.h>

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;
constexpr double kNyquist = kSampleRate * 0.5;

/// Accumulates sum(re^2) and sum(im^2) per band once warm-up blocks have passed.
class EnergyCaptureProcessor : public dtcwpt::BandProcessor
{
public:
    explicit EnergyCaptureProcessor (int warmupBlocks) : warmupBlocks_ (warmupBlocks) {}

    void prepare (double, int, int numBands, int) override
    {
        energyRe.assign (static_cast<size_t> (numBands), 0.0);
        energyIm.assign (static_cast<size_t> (numBands), 0.0);
        blocksSeen_ = 0;
    }

    void reset() override {}

    void processAllBands (dtcwpt::BandData& data) override
    {
        if (blocksSeen_++ < warmupBlocks_)
            return;

        for (int ch = 0; ch < data.numChannels; ++ch)
        {
            for (int b = 0; b < data.numBands; ++b)
            {
                const auto& view = data.bands[static_cast<size_t> (ch)][static_cast<size_t> (b)];
                for (size_t s = 0; s < view.numSamples; ++s)
                {
                    energyRe[static_cast<size_t> (b)] += view.re[s] * view.re[s];
                    energyIm[static_cast<size_t> (b)] += view.im[s] * view.im[s];
                }
            }
        }
    }

    std::vector<double> energyRe, energyIm;

private:
    int warmupBlocks_;
    int blocksSeen_ = 0;
};

struct PeakBands
{
    size_t re = 0;
    size_t im = 0;
};

std::vector<dtcwpt::NormalizedFrequencyRange> destinationRanges (const std::vector<std::string>& destinations)
{
    dtcwpt::TopologyConfig config;
    config.destinations = destinations;

    dtcwpt::DTCWPTProcessor processor;
    processor.prepareToPlay (kSampleRate, 512, config, 1);
    return processor.getDestinationFrequencyRanges();
}

PeakBands measurePeakBands (const std::vector<std::string>& destinations, double frequencyHz)
{
    constexpr int blockSize = 512;
    constexpr int numBlocks = 96;
    constexpr int warmupBlocks = 16;

    dtcwpt::TopologyConfig config;
    config.destinations = destinations;

    dtcwpt::DTCWPTProcessor processor;
    processor.prepareToPlay (kSampleRate, blockSize, config, 1);

    auto* capture = new EnergyCaptureProcessor (warmupBlocks);
    processor.setBandProcessor (std::unique_ptr<dtcwpt::BandProcessor> (capture));

    juce::AudioBuffer<double> buffer (1, blockSize);
    long long n = 0;
    for (int blk = 0; blk < numBlocks; ++blk)
    {
        for (int i = 0; i < blockSize; ++i, ++n)
            buffer.setSample (0, i, 0.5 * std::sin (2.0 * kPi * frequencyHz * static_cast<double> (n) / kSampleRate));
        processor.processBlock (buffer);
    }

    auto argmax = [] (const std::vector<double>& v)
    {
        size_t best = 0;
        for (size_t i = 1; i < v.size(); ++i)
            if (v[i] > v[best])
                best = i;
        return best;
    };

    return { argmax (capture->energyRe), argmax (capture->energyIm) };
}

} // namespace

class FrequencyOrderTests : public juce::UnitTest
{
public:
    FrequencyOrderTests() : UnitTest ("FrequencyOrderTests") {}

    void runTest() override
    {
        beginTest ("full packet depth 3: sine energy peaks in the band reported for that frequency");
        checkTopologyAtBandCentres ({ "LLL", "LLH", "LHL", "LHH", "HLL", "HLH", "HHL", "HHH" });

        beginTest ("full packet depth 4: sine energy peaks in the band reported for that frequency");
        {
            std::vector<std::string> dests;
            for (int i = 0; i < 16; ++i)
            {
                std::string p;
                for (int bit = 3; bit >= 0; --bit)
                    p += ((i >> bit) & 1) ? 'H' : 'L';
                dests.push_back (p);
            }
            // Bands are 1.5 kHz wide here and the level-1 filter's transition band around
            // 12 kHz is wider than half a band, so probe 70% into each band instead of its centre.
            checkTopologyAtBandCentres (dests, 0.7);
        }

        beginTest ("DWT depth 5 (mixed depths): sine energy peaks in the band reported for that frequency");
        checkTopologyAtBandCentres ({ "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLL" });

        beginTest ("mixed topology under H: sine energy peaks in the band reported for that frequency");
        checkTopologyAtBandCentres ({ "L", "HLL", "HLH", "HHL", "HHH" });

        beginTest ("depth 3 measurement table from issue #12");
        {
            const std::vector<std::string> dests { "LLL", "LLH", "LHL", "LHH", "HLL", "HLH", "HHL", "HHH" };
            const std::vector<std::pair<double, std::string>> expected {
                { 1500.0, "LLL" }, { 4500.0, "LLH" }, { 7500.0, "LHH" }, { 10500.0, "LHL" },
                { 13500.0, "HHL" }, { 16500.0, "HHH" }, { 19500.0, "HLH" }, { 22500.0, "HLL" } };

            for (const auto& [freq, path] : expected)
            {
                const auto peak = measurePeakBands (dests, freq);
                expectEquals (dests[peak.re], path, juce::String (freq) + " Hz (Re tree)");
                expectEquals (dests[peak.im], path, juce::String (freq) + " Hz (Im tree)");
            }
        }
    }

private:
    void checkTopologyAtBandCentres (const std::vector<std::string>& dests, double position = 0.5)
    {
        const auto ranges = destinationRanges (dests);
        expectEquals (static_cast<int> (ranges.size()), static_cast<int> (dests.size()));
        if (ranges.size() != dests.size())
            return;

        for (size_t i = 0; i < dests.size(); ++i)
        {
            const auto& path = dests[i];

            // The engine's per-destination ranges agree with the per-path lookup.
            const auto byPath = dtcwpt::getNormalizedFrequencyRange (path);
            expectEquals (ranges[i].lo, byPath.lo);
            expectEquals (ranges[i].hi, byPath.hi);

            const double tone = (ranges[i].lo + position * (ranges[i].hi - ranges[i].lo)) * kNyquist;
            const auto peak = measurePeakBands (dests, tone);

            expectEquals (dests[peak.re], path,
                          "tone in " + juce::String (path) + " (" + juce::String (tone)
                          + " Hz), Re tree peak band");
            expectEquals (dests[peak.im], path,
                          "tone in " + juce::String (path) + " (" + juce::String (tone)
                          + " Hz), Im tree peak band");
        }
    }
};

static FrequencyOrderTests frequencyOrderTests;
