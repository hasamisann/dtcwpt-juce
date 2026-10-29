/**
 * @file dtcwpt_frequency_mapping.ipp
 * @brief Implementation of the path / node ID to frequency band mapping.
 */

#include "dtcwpt_frequency_mapping.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace dtcwpt {

namespace {

/// Accumulates the Gray-code walk one path step at a time.
struct FrequencyWalk {
    std::uint64_t index = 0;  ///< Ascending frequency index within the current depth
    int depth = 0;
    bool mirrored = false;

    void step(bool isHigh) noexcept {
        index = (index << 1) | ((isHigh != mirrored) ? 1u : 0u);
        if (isHigh) {
            mirrored = !mirrored;
        }
        ++depth;
    }

    NormalizedFrequencyRange range() const noexcept {
        const double width = std::ldexp(1.0, -depth);
        const double lo = static_cast<double>(index) * width;
        return { lo, lo + width };
    }
};

FrequencyWalk walkPath(const std::string& path) noexcept {
    FrequencyWalk walk;
    for (const char ch : path) {
        walk.step(ch != 'L');
    }
    return walk;
}

FrequencyWalk walkNodeId(int nodeId) noexcept {
    FrequencyWalk walk;
    if (nodeId < 1) {
        return walk;
    }

    const unsigned id = static_cast<unsigned>(nodeId);
    for (int bit = std::bit_width(id) - 2; bit >= 0; --bit) {
        walk.step(((id >> bit) & 1u) != 0u);
    }
    return walk;
}

} // namespace

NormalizedFrequencyRange getNormalizedFrequencyRange(const std::string& path) noexcept {
    return walkPath(path).range();
}

NormalizedFrequencyRange getNormalizedFrequencyRange(int nodeId) noexcept {
    return walkNodeId(nodeId).range();
}

std::uint64_t getFrequencyOrderIndex(const std::string& path) noexcept {
    return walkPath(path).index;
}

std::uint64_t getFrequencyOrderIndex(int nodeId) noexcept {
    return walkNodeId(nodeId).index;
}

void sortPathsByFrequency(std::vector<std::string>& paths, bool descending) {
    std::stable_sort(paths.begin(), paths.end(),
                     [descending](const std::string& a, const std::string& b) {
                         const double loA = getNormalizedFrequencyRange(a).lo;
                         const double loB = getNormalizedFrequencyRange(b).lo;
                         return descending ? (loA > loB) : (loA < loB);
                     });
}

} // namespace dtcwpt
