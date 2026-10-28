/**
 * @file SynthesisPolyphaseTests.cpp
 * @brief JUCE UnitTest suite for SynthesisNode cost and numerical equivalence (issue #18).
 *
 * - The multiply-accumulate count per output sample must reflect polyphase
 *   synthesis: the zero-inserted half of the stream and the zero edge taps of
 *   the coefficient arrays are never multiplied.
 * - The output must be bit-identical to the straightforward zero-insertion
 *   implementation kept in tests/util/LegacyDsp.h.
 */

#include <dtcwpt/dtcwpt_filter_coeffs.h>
#include <dtcwpt/dtcwpt_synthesis_node.h>
#include "../util/LegacyDsp.h"
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

struct NamedCoefs {
    const char* name;
    const dtcwpt::filters::FilterCoefficients* coefs;
};

const std::vector<NamedCoefs>& allFilterSets() {
    static const std::vector<NamedCoefs> sets = {
        {"CDF_RE", &dtcwpt::filters::CDF_RE},
        {"CDF_IM", &dtcwpt::filters::CDF_IM},
        {"QSHIFT14_RE", &dtcwpt::filters::QSHIFT14_RE},
        {"QSHIFT14_IM", &dtcwpt::filters::QSHIFT14_IM},
        {"PACKET", &dtcwpt::filters::PACKET},
    };
    return sets;
}

/// Length of the span from the first to the last non-zero element (0 if all zero).
size_t nonZeroSpan(const std::vector<double>& v) {
    size_t first = v.size(), last = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != 0.0) {
            if (first == v.size()) first = i;
            last = i;
        }
    }
    return first == v.size() ? 0 : last - first + 1;
}

/// Multiplies needed per input sample (= per pair of output samples) by a
/// polyphase filter: even-index taps for the even output, odd-index taps for
/// the odd output, each without its leading/trailing zeros.
size_t polyphaseMacsPerInputSample(const std::vector<double>& kernel) {
    std::vector<double> even, odd;
    for (size_t i = 0; i < kernel.size(); ++i) (i % 2 == 0 ? even : odd).push_back(kernel[i]);
    return nonZeroSpan(even) + nonZeroSpan(odd);
}

bool sameBits(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

} // namespace

class SynthesisPolyphaseTests : public juce::UnitTest {
public:
    SynthesisPolyphaseTests() : UnitTest("SynthesisPolyphase") {}

    void runTest() override {
        beginTest("MAC count per output sample excludes zero-inserted samples and zero edge taps");
        testMacCount();

        beginTest("Output is bit-identical to the zero-insertion reference");
        testBitExactAgainstReference();
    }

private:
    void testMacCount() {
#if DTCWPT_ENABLE_TEST_SEAMS
        constexpr size_t numInputSamples = 64;
        std::vector<double> low(numInputSamples, 0.5), high(numInputSamples, -0.25);
        std::vector<double> out(2 * numInputSamples, 0.0);

        for (const auto& set : allFilterSets()) {
            dtcwpt::SynthesisNode node(*set.coefs, false);
            node.resetMacCountForTesting();
            node.synthesizeBlock(low, high, out, 0, numInputSamples);

            const unsigned long long actual = node.getMacCountForTesting();
            const unsigned long long expected =
                numInputSamples * (polyphaseMacsPerInputSample(set.coefs->g0)
                                   + polyphaseMacsPerInputSample(set.coefs->g1));
            const unsigned long long fullTaps =
                2 * numInputSamples * (set.coefs->g0.size() + set.coefs->g1.size());

            expectEquals(actual, expected,
                         juce::String(set.name) + ": multiply-accumulates for "
                             + juce::String(static_cast<int>(2 * numInputSamples))
                             + " output samples (all-tap convolution would need "
                             + juce::String(static_cast<juce::int64>(fullTaps)) + ")");
            expect(actual * 100 <= fullTaps * 55,
                   juce::String(set.name) + ": polyphase synthesis should need about half of the full-tap multiplies");
        }
#else
        expect(false, "DTCWPT_ENABLE_TEST_SEAMS must be enabled for MAC counting");
#endif
    }

    void testBitExactAgainstReference() {
        std::mt19937 rng(TestUtils::TestConfig::RANDOM_SEED);
        std::uniform_real_distribution<double> value(-1.0, 1.0);
        std::uniform_int_distribution<int> zeroDist(0, 6);
        const std::vector<size_t> blockSizes = {1, 7, 64, 3, 128, 1, 31, 256, 5};

        for (const auto& set : allFilterSets()) {
            constexpr size_t total = 2048;
            std::vector<double> low(total), high(total);
            for (size_t i = 0; i < total; ++i) {
                low[i] = zeroDist(rng) == 0 ? 0.0 : value(rng);
                high[i] = zeroDist(rng) == 0 ? -0.0 : value(rng);
            }

            // Reference: whole stream in one call.
            LegacyDsp::SynthesisNode reference(set.coefs->g0, set.coefs->g1);
            std::vector<double> expected(2 * total);
            reference.synthesizeBlock(low.data(), high.data(), expected.data(), total);

            // Engine: same stream in irregular blocks, written at an offset.
            dtcwpt::SynthesisNode node(*set.coefs, false);
            std::vector<double> actual(2 * total + 16, 12345.0);
            size_t pos = 0;
            size_t blockIdx = 0;
            while (pos < total) {
                const size_t n = std::min(blockSizes[blockIdx++ % blockSizes.size()], total - pos);
                const std::vector<double> lowBlock(low.begin() + static_cast<std::ptrdiff_t>(pos),
                                                   low.begin() + static_cast<std::ptrdiff_t>(pos + n));
                const std::vector<double> highBlock(high.begin() + static_cast<std::ptrdiff_t>(pos),
                                                    high.begin() + static_cast<std::ptrdiff_t>(pos + n));
                node.synthesizeBlock(lowBlock, highBlock, actual, 8 + 2 * pos, n);
                pos += n;
            }

            int mismatches = 0;
            for (size_t i = 0; i < 2 * total; ++i) {
                if (!sameBits(actual[8 + i], expected[i])) ++mismatches;
            }
            expectEquals(mismatches, 0,
                         juce::String(set.name) + ": output samples differing from the reference");
            expectEquals(node.getFilterLowDelay(), set.coefs->d0, juce::String(set.name) + ": low delay metadata");
            expectEquals(node.getFilterHighDelay(), set.coefs->d1, juce::String(set.name) + ": high delay metadata");
        }
    }
};

static SynthesisPolyphaseTests synthesisPolyphaseTests;
