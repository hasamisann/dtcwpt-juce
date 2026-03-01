/**
 * @file ComplexUtils.cpp
 * @brief Implementation of complex utility functions.
 */

#include "dtcwpt_complex_utils.h"

#include <cmath>

namespace dtcwpt {

void complexToMagPhase(const double* re, const double* im,
                       double* mag, double* phase, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        mag[i] = std::sqrt(re[i] * re[i] + im[i] * im[i]);
        phase[i] = std::atan2(im[i], re[i]);
    }
}

void magPhaseToComplex(const double* mag, const double* phase,
                       double* re, double* im, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        // Save values first to support in-place operation (mag==re, phase==im)
        double m = mag[i];
        double p = phase[i];
        re[i] = m * std::cos(p);
        im[i] = m * std::sin(p);
    }
}

} // namespace dtcwpt
