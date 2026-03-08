#include <juce_core/juce_core.h>

#include "AnalyzerLatencyAligner.h"

class AnalyzerLatencyAlignerTests : public juce::UnitTest
{
public:
    AnalyzerLatencyAlignerTests() : juce::UnitTest ("AnalyzerLatencyAlignerTests") {}

    void runTest() override
    {
        beginTest ("Delay=0 passes input through");
        testZeroDelayPassThrough();

        beginTest ("Delay=3 emits zero then prior samples across blocks");
        testFixedDelayAcrossBlocks();

        beginTest ("Delay larger than block size keeps leading zeros");
        testDelayLargerThanBlock();

        beginTest ("Changing delay clears stale history");
        testDelayChangeClearsHistory();

        beginTest ("Delay clamping obeys [0, maxDelay]");
        testDelayClamping();
    }

private:
    void testZeroDelayPassThrough()
    {
        AnalyzerLatencyAligner aligner;
        aligner.prepare (8, 16);
        aligner.setDelaySamples (0);

        const float input[]  = { 1.0f, 2.0f, 3.0f, 4.0f };
        float output[4]      = {};
        aligner.processBlock (input, output, 4);

        for (int i = 0; i < 4; ++i)
            expectEquals (output[i], input[i]);
    }

    void testFixedDelayAcrossBlocks()
    {
        AnalyzerLatencyAligner aligner;
        aligner.prepare (8, 16);
        aligner.setDelaySamples (3);

        const float inputA[] = { 10.0f, 11.0f, 12.0f, 13.0f };
        const float inputB[] = { 20.0f, 21.0f, 22.0f, 23.0f };
        float outputA[4] = {};
        float outputB[4] = {};

        aligner.processBlock (inputA, outputA, 4);
        aligner.processBlock (inputB, outputB, 4);

        const float expectedA[] = { 0.0f, 0.0f, 0.0f, 10.0f };
        const float expectedB[] = { 11.0f, 12.0f, 13.0f, 20.0f };

        for (int i = 0; i < 4; ++i)
            expectEquals (outputA[i], expectedA[i]);

        for (int i = 0; i < 4; ++i)
            expectEquals (outputB[i], expectedB[i]);
    }

    void testDelayLargerThanBlock()
    {
        AnalyzerLatencyAligner aligner;
        aligner.prepare (4, 16);
        aligner.setDelaySamples (8);

        const float inputA[] = { 1.0f, 2.0f, 3.0f, 4.0f };
        const float inputB[] = { 5.0f, 6.0f, 7.0f, 8.0f };
        const float inputC[] = { 9.0f, 10.0f, 11.0f, 12.0f };
        float outputA[4] = {};
        float outputB[4] = {};
        float outputC[4] = {};

        aligner.processBlock (inputA, outputA, 4);
        aligner.processBlock (inputB, outputB, 4);
        aligner.processBlock (inputC, outputC, 4);

        const float expectedA[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float expectedB[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float expectedC[] = { 1.0f, 2.0f, 3.0f, 4.0f };

        for (int i = 0; i < 4; ++i)
            expectEquals (outputA[i], expectedA[i]);

        for (int i = 0; i < 4; ++i)
            expectEquals (outputB[i], expectedB[i]);

        for (int i = 0; i < 4; ++i)
            expectEquals (outputC[i], expectedC[i]);
    }

    void testDelayChangeClearsHistory()
    {
        AnalyzerLatencyAligner aligner;
        aligner.prepare (8, 16);
        aligner.setDelaySamples (5);

        const float first[] = { 2.0f, 4.0f, 6.0f, 8.0f };
        float firstOut[4] = {};
        aligner.processBlock (first, firstOut, 4);

        aligner.setDelaySamples (0);
        const float second[] = { 9.0f, 10.0f, 11.0f, 12.0f };
        float secondOut[4] = {};
        aligner.processBlock (second, secondOut, 4);

        for (int i = 0; i < 4; ++i)
            expectEquals (secondOut[i], second[i]);
    }

    void testDelayClamping()
    {
        AnalyzerLatencyAligner aligner;
        aligner.prepare (4, 6);

        aligner.setDelaySamples (-3);
        expectEquals (aligner.getDelaySamples(), 0);

        aligner.setDelaySamples (999);
        expectEquals (aligner.getDelaySamples(), 6);

        const float input[] = { 1.0f, 2.0f, 3.0f, 4.0f };
        float output[4] = {};
        aligner.processBlock (input, output, 4);

        const float expected[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 4; ++i)
            expectEquals (output[i], expected[i]);
    }
};

static AnalyzerLatencyAlignerTests analyzerLatencyAlignerTests;
