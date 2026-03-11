#pragma once

#include "dtcwpt_topology_limits.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace dtcwpt {

/**
 * @brief Manages the wavelet packet tree structure and processing order.
 * 
 * Parses destination paths (e.g., "LLH", "HL") and generates the necessary
 * node indices for analysis and synthesis processing. Uses 1-based indexing
 * where root = 1, left child = 2*i, right child = 2*i + 1.
 * 
 * Supports tree depths up to 12 (4096 leaves at depth 12).
 */
class TopologyPlanner {
public:
    /// Compatibility alias for supported node-addressable planner storage.
    static constexpr std::size_t MAX_SIZE = topology_limits::kPlannerArraySize;
    
    /// Analysis processing order (parents before children)
    std::vector<int> analysisOrder;

    /// Synthesis processing order (children before parents)
    std::vector<int> synthesisOrder;
    
    /// Destination node indices
    std::vector<int> destinations;

    /**
     * @brief Constructs a TopologyPlanner from destination path strings.
     *
     * Throws std::invalid_argument if any path implies a node index outside the
     * supported depth-12 planner storage.
     */
    TopologyPlanner(const std::vector<std::string>& destPaths);

    /**
     * @brief Returns analysis and synthesis processing order.
     */
    std::pair<std::vector<int>, std::vector<int>> getPlan() const;
    
private:
    /**
     * @brief Builds the topology from destination paths.
     *
     * Marks all nodes from destinations up to root as active,
     * then builds analysisOrder and synthesisOrder.
     */
    void buildTopology();

    // 1-based binary tree indexing: root=1, left child=2i, right child=2i+1.
    // Each path character shifts the index left and ORs 1 for 'H', 0 for 'L'.
    int pathToIndex(const std::string& path) const;
    
    std::vector<std::string> destPaths_;
    std::vector<bool> isActive_;
};

} // namespace dtcwpt
