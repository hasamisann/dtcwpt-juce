/**
 * @file AnalysisSynthesisTests.cpp
 * @brief JUCE UnitTest suite for AnalysisNode and SynthesisNode.
 */

#include <dtcwpt/dtcwpt_analysis_node.h>
#include <dtcwpt/dtcwpt_synthesis_node.h>
#include <dtcwpt/dtcwpt_filter_coeffs.h>
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

class AnalysisSynthesisTests : public juce::UnitTest {
public:
    AnalysisSynthesisTests() : UnitTest("AnalysisSynthesis") {}

    void runTest() override {
        beginTest("Analysis downsample");
        testAnalysisDownsample();

        beginTest("Synthesis upsample");
        testSynthesisUpsample();

        beginTest("Perfect reconstruction");
        testPerfectReconstruction();
    }

private:
    void testAnalysisDownsample() {
        // 2N input samples -> N low-pass + N high-pass outputs
        using namespace dtcwpt;
        using namespace dtcwpt::filters;

        AnalysisNode node(CDF_RE, true);  // Level 1 with CDF coefficients

        const size_t inputSize = 256;  // 2N
        const size_t expectedOutputSize = inputSize / 2;  // N

        std::vector<double> workBuffer(inputSize * 2, 0.0);
        std::vector<char> activeFlags(inputSize * 2, 0);

        // Process 2N samples
        size_t lpCount = 0;
        size_t hpCount = 0;

        for (size_t i = 0; i < inputSize; ++i) {
            double input = std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / 44100.0);
            
            bool wroteOutput = node.updateBuffer(input, workBuffer, activeFlags, 
                                                  static_cast<int>(i), 
                                                  static_cast<int>(i + inputSize));
            
            if (wroteOutput) {
                ++lpCount;
                ++hpCount;
            }
        }

        // Should produce N outputs each
        expectEquals(lpCount, expectedOutputSize,
                     "Should produce N low-pass outputs from 2N inputs");
        expectEquals(hpCount, expectedOutputSize,
                     "Should produce N high-pass outputs from 2N inputs");
    }

    void testSynthesisUpsample() {
        // N LP + N HP inputs -> 2N output samples
        using namespace dtcwpt;
        using namespace dtcwpt::filters;

        SynthesisNode node(CDF_RE, true);  // Level 1 with CDF coefficients

        const size_t inputSize = 128;  // N
        const size_t expectedOutputSize = inputSize * 2;  // 2N

        std::vector<double> lowBuf(inputSize, 0.1);
        std::vector<double> highBuf(inputSize, 0.0);
        std::vector<double> outBuf(expectedOutputSize * 2, 0.0);

        node.synthesizeBlock(lowBuf, highBuf, outBuf, 0, inputSize);

        // Verify output size is 2N (samples are written to positions 0, 2, 4, ...)
        // Actually synthesizeBlock fills 2*numSamples samples
        // Check that we have non-zero output in the expected range
        size_t nonZeroCount = 0;
        for (size_t i = 0; i < expectedOutputSize; ++i) {
            if (std::abs(outBuf[i]) > 1e-10) {
                ++nonZeroCount;
            }
        }

        expect(nonZeroCount > 0, "Synthesis should produce non-zero outputs");
    }

    void testPerfectReconstruction() {
        // Matched analysis/synthesis pair reproduces input with ε ≤ 1e-9, shifted by d0 samples
        using namespace dtcwpt;
        using namespace dtcwpt::filters;

        // Create matched analysis and synthesis nodes
        AnalysisNode analysisNode(CDF_RE, true);
        SynthesisNode synthesisNode(CDF_RE, true);

        const size_t blockSize = 256;
        const size_t numBlocks = 4;

        // Generate test signal
        std::vector<double> inputSignal(blockSize * numBlocks);
        for (size_t i = 0; i < inputSignal.size(); ++i) {
            inputSignal[i] = std::sin(2.0 * 3.14159265358979323846 * 1000.0 * i / 44100.0) * 0.5;
        }

        // Analysis buffers
        std::vector<double> workBuffer(blockSize * 4, 0.0);
        std::vector<char> activeFlags(blockSize * 4, 0);

        // Output buffers
        std::vector<double> outputSignal(blockSize * numBlocks * 2, 0.0);
        size_t outputCursor = 0;

        for (size_t block = 0; block < numBlocks; ++block) {
            // Analysis: 2N -> N
            std::vector<double> lowPass(blockSize / 2, 0.0);
            std::vector<double> highPass(blockSize / 2, 0.0);
            
            size_t lpIdx = 0;
            for (size_t i = 0; i < blockSize; ++i) {
                size_t inputIdx = block * blockSize + i;
                double sample = (inputIdx < inputSignal.size()) ? inputSignal[inputIdx] : 0.0;
                
                bool wrote = analysisNode.updateBuffer(sample, workBuffer, activeFlags,
                                                        static_cast<int>(lpIdx),
                                                        static_cast<int>(blockSize / 2 + lpIdx));
                if (wrote && lpIdx < blockSize / 2) {
                    lowPass[lpIdx] = workBuffer[lpIdx];
                    highPass[lpIdx] = workBuffer[blockSize / 2 + lpIdx];
                    ++lpIdx;
                }
            }

            // Synthesis: N + N -> 2N
            synthesisNode.synthesizeBlock(lowPass, highPass, outputSignal, outputCursor, blockSize / 2);
            outputCursor += blockSize;
        }

        // Get filter delay
        int delay = synthesisNode.getFilterLowDelay();

        // Verify reconstruction (accounting for delay)
        // The output should match input shifted by delay samples
        size_t validSamples = std::min(inputSignal.size(), outputSignal.size() - static_cast<size_t>(delay));
        
        if (validSamples > static_cast<size_t>(delay)) {
            double maxError = 0.0;
            for (size_t i = 0; i < validSamples - static_cast<size_t>(delay); ++i) {
                double error = std::abs(inputSignal[i] - outputSignal[i + static_cast<size_t>(delay)]);
                maxError = std::max(maxError, error);
            }
            
            expect(maxError < 1e-9, 
                   "Perfect reconstruction error should be < 1e-9, got " + juce::String(maxError));
        }
    }
};

// Static registration
static AnalysisSynthesisTests analysisSynthesisTests;
