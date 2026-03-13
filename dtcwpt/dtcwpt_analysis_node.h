#pragma once

#include "dtcwpt_stateful_filter.h"
#include "dtcwpt_filter_structs.h"

#include <cstddef>
#include <vector>

namespace dtcwpt {

/**
 * @brief Child outputs from one analysis arrival.
 */
struct AnalysisChildOutputs {
    double low = 0.0;
    double high = 0.0;
};

/**
 * @brief Analysis node for DT-CWPT decomposition.
 *
 * Each analysis node performs:
 * - Low-pass filtering (filterLow_)
 * - High-pass filtering (filterHigh_)
 * - 2:1 downsampling (controlled by parity flag)
 *
 * The parity flag controls the downsampling: on even samples (parity=false),
 * filter outputs are calculated and written to the work buffer. On odd samples
 * (parity=true), the calculation is skipped.
 */
class AnalysisNode {
public:
    /**
     * @brief Default constructor (required for vector resize).
     */
    AnalysisNode() = default;

    /**
     * @brief Constructs an AnalysisNode with given coefficients.
     *
     * @param coefs Filter coefficients containing analysis filters (h0, h1)
     * @param isLevel1 True for Level 1 (CDF), false for Level >= 2
     */
    AnalysisNode(const filters::FilterCoefficients& coefs, bool isLevel1);

    /**
     * @brief Processes a sample through the analysis filters.
     *
     * Pushes the sample to both filters, then performs 2:1 downsampling
     * based on the parity flag. On even samples, calculates filter outputs
     * and writes to workBuffer. On odd samples, skips calculation.
     *
     * Contract: On even samples (parity=false), writes low-pass and high-pass
     * filter outputs to workBuffer at lowIdx and highIdx, marks those indices
     * as active, and returns true. On odd samples, returns false without
     * writing to workBuffer.
     *
     * @param x Input sample value
     * @param workBuffer Buffer to write filter outputs
     * @param activeFlags Flags indicating which buffer positions are active
     * @param lowIdx Index in workBuffer for low-pass output
     * @param highIdx Index in workBuffer for high-pass output
     * @return true if output was written (even sample), false if skipped (odd sample)
     */
    bool updateBuffer(double x,
                      std::vector<double>& workBuffer,
                      std::vector<char>& activeFlags,
                      int lowIdx,
                      int highIdx);

    /**
     * @brief Direct-return arrival processing API for c08 worklist scheduler.
     *
     * Processes one arrived sample and returns low/high child outputs directly
     * without requiring planner-array-sized scratch buffers.
     *
     * Contract: On emitting arrivals (parity=false), writes low/high outputs
     * to the provided struct and returns true. On non-emitting arrivals
     * (parity=true), returns false and leaves outputs unchanged.
     *
     * @param sampleValue The transported input sample value
     * @param outputs Output struct to receive low/high child values
     * @return true if the node emitted children on this arrival, false if suppressed
     */
    bool processArrival(double sampleValue, AnalysisChildOutputs& outputs) noexcept;

private:
    StatefulFilter filterLow_;   ///< Low-pass analysis filter
    StatefulFilter filterHigh_;  ///< High-pass analysis filter
    bool parity_;                ///< Downsample control flag (false=even, true=odd)
};

} // namespace dtcwpt
