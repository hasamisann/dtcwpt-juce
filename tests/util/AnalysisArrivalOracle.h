/**
 * @file AnalysisArrivalOracle.h
 * @brief Test-only oracle adapter for legacy AnalysisNode::updateBuffer(...) path.
 * 
 * This adapter provides an independent baseline for verifying the new production
 * AnalysisNode::processArrival(...) API by calling the legacy updateBuffer(...)
 * contract directly with dedicated tiny local scratch.
 */

#pragma once

#include "../../dtcwpt/dtcwpt_analysis_node.h"

namespace dtcwpt {
namespace test {

/**
 * @brief Test-only oracle that extracts low/high outputs through the legacy
 *        AnalysisNode::updateBuffer(...) contract.
 * 
 * This adapter uses only dedicated tiny local scratch and never requires
 * planner-array-sized buffers. It must not call AnalysisNode::processArrival(...).
 * 
 * @param node The analysis node to process
 * @param sampleValue The transported input sample value
 * @param outputs Output struct to receive low/high child values
 * @return true if the node emitted children on this arrival, false if suppressed
 */
bool runLegacyAnalysisArrival(
    AnalysisNode& node,
    double sampleValue,
    dtcwpt::AnalysisChildOutputs& outputs);

/**
 * @brief Reset the legacy adapter call counter for a new test scenario.
 */
void resetLegacyAdapterCallCount();

/**
 * @brief Get the number of times runLegacyAnalysisArrival(...) was called
 *        since the last reset.
 */
int getLegacyAdapterCallCount();

} // namespace test
} // namespace dtcwpt
