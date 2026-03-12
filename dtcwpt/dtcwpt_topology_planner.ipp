#include "dtcwpt_topology_planner.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

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
        if (idx < 0 || static_cast<std::size_t>(idx) >= topology_limits::kPlannerArraySize) {
            throw std::invalid_argument("Destination path exceeds supported topology depth");
        }

        isActive_[static_cast<size_t>(idx)] = true;

        int node = idx;
        while (node >= 1) {
            isActive_[static_cast<size_t>(node)] = true;
            node /= 2;  // Move to parent
        }
    }

    for (std::size_t i = 1; i < topology_limits::kPlannerArraySize; ++i) {
        const std::size_t leftChild = i << 1;
        if (leftChild >= topology_limits::kPlannerArraySize || !isActive_[i]) {
            continue;
        }

        assert(isActive_[leftChild] == isActive_[leftChild + 1]
               && "TopologyPlanner expects already-validated full binary topology");
    }

    // Build analysisOrder: forward linear scan over node indices; include inner nodes only
    analysisOrder.clear();
    for (std::size_t i = 1; i < topology_limits::kPlannerArraySize; ++i) {
        if (isActive_[i]) {
            const std::size_t leftChild = i << 1;
            if (leftChild < topology_limits::kPlannerArraySize && isActive_[leftChild]) {
                analysisOrder.push_back(static_cast<int>(i));
            }
        }
    }

    // Build synthesisOrder: children before parents
    // Iterate from MAX_SIZE-1 down to 1
    // Include only nodes that have active leftChild (inner nodes)
    synthesisOrder.clear();
    for (int i = static_cast<int>(topology_limits::kPlannerArraySize) - 1; i > 0; --i) {
        if (isActive_[static_cast<size_t>(i)]) {
            const std::size_t leftChild = static_cast<std::size_t>(i) << 1;
            if (leftChild < topology_limits::kPlannerArraySize && isActive_[leftChild]) {
                synthesisOrder.push_back(i);
            }
        }
    }

}

} // namespace dtcwpt
