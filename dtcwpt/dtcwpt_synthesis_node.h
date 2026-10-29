#pragma once

#include "dtcwpt_stateful_filter.h"
#include "dtcwpt_filter_structs.h"

#include <cstddef>
#include <vector>

namespace dtcwpt {

/**
 * @brief Synthesis node for DT-CWPT reconstruction.
 *
 * Each synthesis node takes Low and High subband inputs, performs
 * upsampling by 2:1 (zero insertion), filters, and sums to produce output.
 * This is the reverse of the analysis/downsampling operation.
 *
 * The zero insertion is never materialised: each synthesis filter is split
 * into its even-index and odd-index taps (polyphase decomposition) that run
 * on the subband samples directly, which halves the multiplications.
 */
class SynthesisNode {
public:
    /**
     * @brief Default constructor (required for vector resize).
     */
    SynthesisNode() = default;

    /**
     * @brief Constructs a SynthesisNode with given coefficients.
     *
     * @param coefs Filter coefficients containing synthesis filters (g0, g1)
     * @param isLevel1 True for Level 1 (CDF), false for Level >= 2
     */
    SynthesisNode(const filters::FilterCoefficients& coefs, bool isLevel1);

    /**
     * @brief Synthesizes a block of samples from low and high subbands.
     *
     * Performs upsampling by 2:1 (zero insertion) on both subbands,
     * filters through synthesis filters, and sums to produce output.
     * The result is that of the zero-insertion form, computed as a
     * polyphase filter bank.
     *
     * Algorithm:
     * For each input sample i (0 to numSamples-1), after pushing lowBuf[i]
     * and highBuf[i] to the phase filters:
     * 1. Even output: even-index taps of g0 and g1
     *    -> outBuf[outCursor + 2*i]
     * 2. Odd output: odd-index taps of g0 and g1
     *    -> outBuf[outCursor + 2*i + 1]
     *
     * @param lowBuf Low subband input buffer
     * @param highBuf High subband input buffer
     * @param outBuf Output buffer (must be large enough)
     * @param outCursor Starting position in output buffer
     * @param numSamples Number of input samples to process
     */
    void synthesizeBlock(const std::vector<double>& lowBuf,
                         const std::vector<double>& highBuf,
                         std::vector<double>& outBuf,
                         size_t outCursor,
                         size_t numSamples);

    /**
     * @brief Returns the low-pass filter delay.
     * @return Delay in samples
     */
    int getFilterLowDelay() const { return delayLow_; }

    /**
     * @brief Returns the high-pass filter delay.
     * @return Delay in samples
     */
    int getFilterHighDelay() const { return delayHigh_; }

#if DTCWPT_ENABLE_TEST_SEAMS
    /// Test seam: total multiply-accumulates executed by this node's filters.
    unsigned long long getMacCountForTesting() const noexcept {
        return filterLowEven_.getMacCountForTesting() + filterLowOdd_.getMacCountForTesting()
             + filterHighEven_.getMacCountForTesting() + filterHighOdd_.getMacCountForTesting();
    }

    /// Test seam: clears the multiply-accumulate counters.
    void resetMacCountForTesting() noexcept {
        filterLowEven_.resetMacCountForTesting();
        filterLowOdd_.resetMacCountForTesting();
        filterHighEven_.resetMacCountForTesting();
        filterHighOdd_.resetMacCountForTesting();
    }
#endif

    double processTick(double lowIn, double highIn);
    void reset();

private:
    bool oddTick_ = false;
    StatefulFilter filterLowEven_;   ///< Even-index taps of the low-pass synthesis filter g0
    StatefulFilter filterLowOdd_;    ///< Odd-index taps of g0
    StatefulFilter filterHighEven_;  ///< Even-index taps of the high-pass synthesis filter g1
    StatefulFilter filterHighOdd_;   ///< Odd-index taps of g1
    int delayLow_ = 0;               ///< Low-pass filter delay (metadata)
    int delayHigh_ = 0;              ///< High-pass filter delay (metadata)
};

} // namespace dtcwpt
