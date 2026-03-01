#pragma once

#include <vector>

namespace dtcwpt::filters {

/**
 * @brief Filter coefficients structure for wavelet filter banks.
 * 
 * Contains analysis filters (h0, h1) and synthesis filters (g0, g1)
 * along with their respective delays (d0, d1).
 */
struct FilterCoefficients {
    std::vector<double> h0;  ///< Low-pass analysis filter
    std::vector<double> h1;  ///< High-pass analysis filter
    std::vector<double> g0;  ///< Low-pass synthesis filter
    std::vector<double> g1;  ///< High-pass synthesis filter
    int d0;                  ///< Low-pass delay (samples)
    int d1;                  ///< High-pass delay (samples)
};

} // namespace dtcwpt::filters
