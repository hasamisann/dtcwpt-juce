/**
 * @file StatefulFilterTests.cpp
 * @brief JUCE UnitTest suite for StatefulFilter.
 */

#include <dtcwpt/dtcwpt_stateful_filter.h>
#include "../util/LegacyDsp.h"
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <bit>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

class StatefulFilterTests : public juce::UnitTest {
public:
    StatefulFilterTests() : UnitTest("StatefulFilter") {}

    void runTest() override {
        beginTest("Unit impulse kernel");
        testUnitImpulseKernel();

        beginTest("Reset clears state");
        testResetClearsState();

        beginTest("Multi-tap FIR correctness");
        testMultiTapFIRCorrectness();

        beginTest("Extended processing beyond kernel length");
        testExtendedProcessing();

        beginTest("Reported delay metadata remains stable across reset boundaries");
        testReportedDelayMetadataRemainsStable();

        beginTest("MAC count: leading and trailing zero taps are not multiplied (issue #18)");
        testZeroEdgeTapsAreNotMultiplied();

        beginTest("Output is bit-identical to the reference implementation (issue #18)");
        testBitExactAgainstReference();
    }

private:
    void testUnitImpulseKernel() {
        // Kernel [1.0], delay=0 -> pushSample(x) -> filtering() returns x
        dtcwpt::StatefulFilter filter({1.0}, 0);

        for (int i = 0; i < 10; ++i) {
            double input = static_cast<double>(i) * 0.1;
            filter.pushSample(input);
            double output = filter.filtering();
            expectWithinAbsoluteError(output, input, TestUtils::TestConfig::EPSILON,
                                       "Unit impulse kernel should pass through input");
        }
    }

    void testResetClearsState() {
        // Create filter with multi-tap kernel
        std::vector<double> kernel = {0.5, 0.3, 0.2};
        dtcwpt::StatefulFilter filter(kernel, 0);

        // Process some samples
        for (int i = 0; i < 10; ++i) {
            filter.pushSample(static_cast<double>(i));
            filter.filtering();
        }

        // Reset
        filter.reset();

        // Create fresh filter and compare
        dtcwpt::StatefulFilter freshFilter(kernel, 0);

        // Process same samples - should get same results
        for (int i = 0; i < 5; ++i) {
            double input = static_cast<double>(i) * 0.5;
            
            filter.pushSample(input);
            freshFilter.pushSample(input);
            
            double output1 = filter.filtering();
            double output2 = freshFilter.filtering();
            
            expectWithinAbsoluteError(output1, output2, TestUtils::TestConfig::EPSILON,
                                       "Reset filter should behave like fresh construction");
        }
    }

    void testMultiTapFIRCorrectness() {
        // Kernel [0.5, 0.3, 0.2]
        // Input:  [1, 2, 3, 4, 5]
        // Output after each pushSample+filtering:
        //   push(1): 0.5*1 = 0.5
        //   push(2): 0.5*2 + 0.3*1 = 1.3
        //   push(3): 0.5*3 + 0.3*2 + 0.2*1 = 2.3
        //   push(4): 0.5*4 + 0.3*3 + 0.2*2 = 3.3
        //   push(5): 0.5*5 + 0.3*4 + 0.2*3 = 4.3
        
        std::vector<double> kernel = {0.5, 0.3, 0.2};
        dtcwpt::StatefulFilter filter(kernel, 0);

        std::vector<double> inputs = {1.0, 2.0, 3.0, 4.0, 5.0};
        std::vector<double> expected = {0.5, 1.3, 2.3, 3.3, 4.3};

        for (size_t i = 0; i < inputs.size(); ++i) {
            filter.pushSample(inputs[i]);
            double output = filter.filtering();
            expectWithinAbsoluteError(output, expected[i], TestUtils::TestConfig::EPSILON,
                                       "Multi-tap FIR output mismatch at index " + juce::String(i));
        }
    }

    void testExtendedProcessing() {
        // Process more samples than kernel length to verify no wrap-around errors
        std::vector<double> kernel = {0.25, 0.5, 0.25};
        dtcwpt::StatefulFilter filter(kernel, 0);

        // Process 100 samples
        for (int i = 0; i < 100; ++i) {
            filter.pushSample(static_cast<double>(i) * 0.01);
            double output = filter.filtering();
            
            // Just verify output is finite and reasonable
            expect(std::isfinite(output), "Output should be finite");
            expect(output >= -1.0 && output <= 1.0, "Output should be in reasonable range");
        }
    }

    void testReportedDelayMetadataRemainsStable() {
        dtcwpt::StatefulFilter filter({0.5, 0.3, 0.2}, 2);
        expectEquals(filter.getDelay(), 2, "Configured delay metadata should be reported before processing");

        for (double sample : {1.0, -0.5, 0.25, -0.125}) {
            filter.pushSample(sample);
            expect(std::isfinite(filter.filtering()), "Continuous processing should remain finite around reset boundaries");
        }

        filter.reset();
        expectEquals(filter.getDelay(), 2, "Reset should not mutate the configured delay metadata");

        filter.pushSample(0.75);
        expect(std::isfinite(filter.filtering()), "Filtering should remain finite immediately after reset");
    }

    // Counts the multiply-accumulates of one filtering() call on a kernel whose
    // first and last taps are zero. The zero edge taps contribute nothing, so
    // only the span from the first to the last non-zero tap needs multiplying.
    void testZeroEdgeTapsAreNotMultiplied() {
#if DTCWPT_ENABLE_TEST_SEAMS
        struct Case {
            std::vector<double> kernel;
            unsigned long long expectedMacsPerCall;
        };
        const std::vector<Case> cases = {
            {{0.0, 0.0, 1.0, 2.0, 3.0, 0.0, 0.0}, 3},
            {{0.5, 0.3, 0.2}, 3},
            {{0.0, 0.5, 0.0, 0.2, 0.0}, 3},   // interior zero stays, edges go
            {{0.0, 0.0, 0.0}, 0},             // all-zero kernel: nothing to multiply
        };

        for (const auto& c : cases) {
            dtcwpt::StatefulFilter filter(c.kernel, 0);
            constexpr int calls = 16;
            for (int i = 0; i < calls; ++i) {
                filter.pushSample(1.0 + i);
                filter.filtering();
            }
            expectEquals(filter.getMacCountForTesting(),
                         c.expectedMacsPerCall * calls,
                         "Multiply-accumulate count for kernel of size "
                             + juce::String(static_cast<int>(c.kernel.size())));
        }
#else
        expect(false, "DTCWPT_ENABLE_TEST_SEAMS must be enabled for MAC counting");
#endif
    }

    static bool sameBits(double a, double b) {
        return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    }

    // The optimised filter must reproduce the reference filter bit for bit,
    // including kernels with zero edge taps and inputs containing +/-0.0.
    void testBitExactAgainstReference() {
        std::mt19937 rng(TestUtils::TestConfig::RANDOM_SEED);
        std::uniform_real_distribution<double> value(-1.0, 1.0);
        std::uniform_int_distribution<int> lengthDist(1, 20);
        std::uniform_int_distribution<int> padDist(0, 3);
        std::uniform_int_distribution<int> zeroInputDist(0, 5);

        int mismatches = 0;
        long long compared = 0;

        for (int trial = 0; trial < 200; ++trial) {
            std::vector<double> kernel(static_cast<size_t>(lengthDist(rng)));
            for (auto& k : kernel) k = value(rng);
            // Random zero padding at both ends (and occasionally -0.0 / interior zeros)
            const int lead = padDist(rng);
            const int trail = padDist(rng);
            kernel.insert(kernel.begin(), static_cast<size_t>(lead), trial % 2 ? -0.0 : 0.0);
            kernel.insert(kernel.end(), static_cast<size_t>(trail), 0.0);
            if (trial % 7 == 0 && kernel.size() > 4) kernel[kernel.size() / 2] = 0.0;
            if (trial % 31 == 0) std::fill(kernel.begin(), kernel.end(), 0.0);

            dtcwpt::StatefulFilter filter(kernel, 0);
            LegacyDsp::StatefulFilter reference(kernel);

            for (int n = 0; n < 300; ++n) {
                double x = value(rng);
                const int z = zeroInputDist(rng);
                if (z == 0) x = 0.0;
                if (z == 1) x = -0.0;
                filter.pushSample(x);
                reference.pushSample(x);
                if (!sameBits(filter.filtering(), reference.filtering())) ++mismatches;
                ++compared;
            }
        }

        expectEquals(mismatches, 0, "Outputs must be bit-identical to the reference ("
                                        + juce::String(compared) + " samples compared)");
    }
};

// Static registration
static StatefulFilterTests statefulFilterTests;
