/**
 * @file StatefulFilterTests.cpp
 * @brief JUCE UnitTest suite for StatefulFilter.
 */

#include <dtcwpt/dtcwpt_stateful_filter.h>
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <cmath>
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
};

// Static registration
static StatefulFilterTests statefulFilterTests;
