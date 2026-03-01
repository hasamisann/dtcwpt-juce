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
     *
     * Algorithm:
     * For each input sample i (0 to numSamples-1):
     * 1. Even sample: push lowBuf[i] and highBuf[i] to respective filters,
     *    sum outputs -> outBuf[outCursor + 2*i]
     * 2. Odd sample: push 0 to both filters,
     *    sum outputs -> outBuf[outCursor + 2*i + 1]
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
    int getFilterLowDelay() const { return filterLow_.getDelay(); }

    /**
     * @brief Returns the high-pass filter delay.
     * @return Delay in samples
     */
    int getFilterHighDelay() const { return filterHigh_.getDelay(); }

private:
    StatefulFilter filterLow_;   ///< Low-pass synthesis filter
    StatefulFilter filterHigh_;  ///< High-pass synthesis filter
};

} // namespace dtcwpt
