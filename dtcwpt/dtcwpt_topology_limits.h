#pragma once

#include <cstddef>

namespace dtcwpt::topology_limits {

inline constexpr int kSupportedMaxDepth = 12;

inline constexpr std::size_t kPlannerArraySize =
    std::size_t{1} << (kSupportedMaxDepth + 1);

} // namespace dtcwpt::topology_limits
