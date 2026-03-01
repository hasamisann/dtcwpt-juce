#include "dtcwpt_topology_planner.h"

#include <algorithm>
#include <cassert>

namespace dtcwpt {

TopologyPlanner::TopologyPlanner(const std::vector<std::string>& destPaths)
    : destPaths_(destPaths)
    , isActive_(MAX_SIZE, false) {

    buildTopology();

    destinations.clear();
    destinations.reserve(destPaths_.size());
    for (const auto& path : destPaths_) {
        destinations.push_back(pathToIndex(path));
    }
}

std::pair<std::vector<int>, std::vector<int>> TopologyPlanner::getPlan() const {
    return {analysisOrder, synthesisOrder};
}

int TopologyPlanner::pathToIndex(const std::string& path) const {
    int index = 1;  // Start at root
    
    for (char c : path) {
        // Left shift (multiply by 2) for next level
        // Add 1 if 'H', 0 if 'L'
        index = (index << 1) | (c == 'H' ? 1 : 0);
    }
    
    return index;
}

void TopologyPlanner::buildTopology() {
    std::fill(isActive_.begin(), isActive_.end(), false);

    // Mark destination and all ancestors active
    for (const auto& path : destPaths_) {
        int idx = pathToIndex(path);
        isActive_[static_cast<size_t>(idx)] = true;

        int node = idx;
        while (node >= 1) {
            isActive_[static_cast<size_t>(node)] = true;
            node /= 2;  // Move to parent
        }
    }

    // Build analysisOrder: forward linear scan over node indices; include inner nodes only
    analysisOrder.clear();
    for (size_t i = 1; i < MAX_SIZE; ++i) {
        if (isActive_[i]) {
            size_t leftChild = i << 1;
            if (leftChild < MAX_SIZE && isActive_[leftChild]) {
                analysisOrder.push_back(static_cast<int>(i));
            }
        }
    }

    // Build synthesisOrder: children before parents
    // Iterate from MAX_SIZE-1 down to 1
    // Include only nodes that have active leftChild (inner nodes)
    synthesisOrder.clear();
    for (int i = static_cast<int>(MAX_SIZE) - 1; i > 0; --i) {
        if (isActive_[static_cast<size_t>(i)]) {
            size_t leftChild = static_cast<size_t>(i) << 1;  // i * 2
            if (leftChild < MAX_SIZE && isActive_[leftChild]) {
                synthesisOrder.push_back(i);
            }
        }
    }

}

} // namespace dtcwpt
