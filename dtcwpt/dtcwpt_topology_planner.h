#pragma once

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
 * Supports tree depths up to 8 (256 leaves at depth 8).
 */
class TopologyPlanner {
public:
    /// Maximum array size (2^10 = 1024)
    static constexpr size_t MAX_SIZE = 1024;
    
    /// Analysis processing order (parents before children)
    std::vector<int> analysisOrder;

    /// Synthesis processing order (children before parents)
    std::vector<int> synthesisOrder;
    
    /// Destination node indices
    std::vector<int> destinations;

    /**
     * @brief Constructs a TopologyPlanner from destination path strings.
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
