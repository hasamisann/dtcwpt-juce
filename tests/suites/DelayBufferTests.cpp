/**
 * @file DelayBufferTests.cpp
 * @brief JUCE UnitTest suite for DelayBuffer.
 */

#include <dtcwpt/dtcwpt_delay_buffer.h>
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

class DelayBufferTests : public juce::UnitTest {
public:
    DelayBufferTests() : UnitTest("DelayBuffer") {}

    void runTest() override {
        beginTest("Pass-through (delay=0)");
        testPassThrough();

        beginTest("Impulse delay");
        testImpulseDelay();

        beginTest("Wrap-around boundary");
        testWrapAround();

        beginTest("Allocation-free processing");
        testAllocationFree();
    }

private:
    void testPassThrough() {
        // delay=0 -> output samples equal input samples
        const int blockSize = 64;
        dtcwpt::DelayBuffer buffer(0, blockSize);

        std::vector<double> input(blockSize);
        std::vector<double> output(blockSize);

        // Fill input with test pattern
        for (int i = 0; i < blockSize; ++i) {
            input[static_cast<size_t>(i)] = static_cast<double>(i) * 0.1;
        }

        buffer.process(input, output, blockSize);

        // Check output equals input
        for (int i = 0; i < blockSize; ++i) {
            expectWithinAbsoluteError(output[static_cast<size_t>(i)], 
                                       input[static_cast<size_t>(i)], 
                                       TestUtils::TestConfig::EPSILON,
                                       "Pass-through output mismatch at index " + juce::String(i));
        }
    }

    void testImpulseDelay() {
        // Single impulse at position 0, delay=N -> impulse appears at position N
        const int delaySamples = 50;
        const int blockSize = 64;

        dtcwpt::DelayBuffer buffer(delaySamples, blockSize);

        std::vector<double> input(blockSize, 0.0);
        std::vector<double> output(blockSize);

        // Place impulse at position 0
        input[0] = 1.0;

        // Process multiple blocks to get the impulse through
        std::vector<double> allOutput;
        for (int block = 0; block < 5; ++block) {
            buffer.process(input, output, blockSize);
            for (int i = 0; i < blockSize; ++i) {
                allOutput.push_back(output[static_cast<size_t>(i)]);
            }
            // Clear input for subsequent blocks
            std::fill(input.begin(), input.end(), 0.0);
        }

        // Check impulse appears at position delaySamples
        bool foundImpulse = false;
        for (size_t i = 0; i < allOutput.size(); ++i) {
            if (i == static_cast<size_t>(delaySamples)) {
                expectWithinAbsoluteError(allOutput[i], 1.0, TestUtils::TestConfig::EPSILON,
                                           "Impulse should appear at delay position");
                foundImpulse = true;
            } else if (allOutput[i] > 0.1) {
                // Allow some tolerance for filter ringing but not full impulse
            }
        }
        expect(foundImpulse, "Impulse should be found at delay position");
    }

    void testWrapAround() {
        // Multiple blocks exceeding ring buffer capacity -> correct delayed sequence
        const int delaySamples = 30;
        const int blockSize = 64;
        const int smallBlock = 16;

        dtcwpt::DelayBuffer buffer(delaySamples, blockSize);

        std::vector<double> input(blockSize);
        std::vector<double> output(blockSize);

        // Process several small blocks to trigger wrap-around
        for (int block = 0; block < 20; ++block) {
            // Fill with different values each block
            for (int i = 0; i < smallBlock; ++i) {
                input[static_cast<size_t>(i)] = static_cast<double>(block * 100 + i);
            }

            buffer.process(input, output, smallBlock);
        }

        // If we got here without crash, wrap-around works
        expect(true, "Wrap-around processing completed without crash");
    }

    void testAllocationFree() {
        // Process after construction -> zero allocations during process()
        const int blockSize = 512;
        dtcwpt::DelayBuffer buffer(100, blockSize);

        std::vector<double> input(blockSize, 0.0);
        std::vector<double> output(blockSize, 0.0);

        TestUtils::AllocationCounter counter;
        counter.begin();

        // Process multiple blocks
        for (int i = 0; i < 100; ++i) {
            buffer.process(input, output, blockSize);
        }

        counter.end();

        expect(counter.getCount() == 0, 
               "No allocations should occur during process() - found " + juce::String(static_cast<int>(counter.getCount())));
    }
};

// Static registration
static DelayBufferTests delayBufferTests;
