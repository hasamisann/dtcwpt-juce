#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dtcwpt {

/**
 * @brief Frequency band of a tree node, normalised so that 0 is DC and 1 is Nyquist.
 */
struct NormalizedFrequencyRange {
    double lo = 0.0;  ///< Lower edge (0 .. 1)
    double hi = 1.0;  ///< Upper edge (0 .. 1)
};

/**
 * @brief Path string / node ID <-> frequency band correspondence.
 *
 * After a high-pass filter and 2:1 decimation the spectrum of the sub-band is
 * mirrored. The children of an "H" node therefore appear in reversed order:
 * the path string is NOT the natural binary expansion of the frequency, and
 * the leaves of a wavelet packet tree are in Gray-code order.
 *
 * Rule, walking the path from the root: while the "mirrored" flag is off,
 * L is the lower half and H the upper half of the current band; while it is
 * on, L is the upper half and H the lower half. Passing through H toggles the
 * flag. (This holds for both the Re and Im trees and for all levels.)
 *
 * Example, depth 3 at 48 kHz (3 kHz per band), lowest to highest:
 * LLL, LLH, LHH, LHL, HHL, HHH, HLH, HLL.
 *
 * Node IDs use the planner numbering (root = 1, L = 2*id, H = 2*id + 1).
 * Characters other than 'L' in a path are treated as 'H'; a node ID < 1 is
 * treated as the root. All functions are noexcept and allocation free except
 * sortPathsByFrequency().
 */

/// Frequency range covered by the sub-band at @p path ("" = whole band).
NormalizedFrequencyRange getNormalizedFrequencyRange(const std::string& path) noexcept;

/// Frequency range covered by the sub-band of @p nodeId.
NormalizedFrequencyRange getNormalizedFrequencyRange(int nodeId) noexcept;

/**
 * @brief Index of the band among all bands of the same depth, ascending in frequency.
 *
 * 0 is the lowest band and 2^depth - 1 the highest. Valid for depth <= 63.
 */
std::uint64_t getFrequencyOrderIndex(const std::string& path) noexcept;

/// @copydoc getFrequencyOrderIndex(const std::string&)
std::uint64_t getFrequencyOrderIndex(int nodeId) noexcept;

/**
 * @brief Sorts destination paths by frequency (stable; ties on the lower edge keep their order).
 *
 * @param paths       Paths to reorder in place.
 * @param descending  true for highest frequency first, false for lowest first.
 */
void sortPathsByFrequency(std::vector<std::string>& paths, bool descending = false);

} // namespace dtcwpt
