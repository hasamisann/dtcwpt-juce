/**
 * @file AnalysisArrivalOracle.cpp
 * @brief Implementation of test-only legacy arrival oracle adapter.
 */

#include "AnalysisArrivalOracle.h"

#include <array>
#include <atomic>

namespace dtcwpt {
namespace test {

namespace {
    std::atomic<int> gLegacyAdapterCallCount{0};
}

bool runLegacyAnalysisArrival(
    AnalysisNode& node,
    double sampleValue,
    dtcwpt::AnalysisChildOutputs& outputs)
{
    // Increment call counter for baseline independence verification
    gLegacyAdapterCallCount.fetch_add(1, std::memory_order_relaxed);

    // Fixed two-slot local scratch for legacy updateBuffer(...) contract
    std::vector<double> scratchValues(2, 0.0);
    std::vector<char> scratchFlags(2, 0);

    // Call legacy API with fixed node IDs 0 (low) and 1 (high)
    node.updateBuffer(sampleValue, scratchValues, scratchFlags, 0, 1);

    // Extract outputs
    outputs.low = scratchValues[0];
    outputs.high = scratchValues[1];

    // Emit decision: true if either child was marked active
    return scratchFlags[0] != 0 || scratchFlags[1] != 0;
}

void resetLegacyAdapterCallCount()
{
    gLegacyAdapterCallCount.store(0, std::memory_order_relaxed);
}

int getLegacyAdapterCallCount()
{
    return gLegacyAdapterCallCount.load(std::memory_order_relaxed);
}

} // namespace test
} // namespace dtcwpt
