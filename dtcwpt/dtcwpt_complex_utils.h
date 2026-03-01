#pragma once

#include <cstddef>

namespace dtcwpt {

/**
 * @brief Convert complex Re/Im arrays to magnitude/phase arrays.
 *
 * Computes: mag[i] = sqrt(re[i]^2 + im[i]^2)
 *           phase[i] = atan2(im[i], re[i])
 *
 * In-place operation is supported (re==mag and/or im==phase is valid).
 * Each element is processed independently, so no temporary buffers are needed.
 *
 * @param re     Input real part array
 * @param im     Input imaginary part array
 * @param mag    Output magnitude array (may be same as re for in-place)
 * @param phase  Output phase array (may be same as im for in-place)
 * @param n      Number of elements to process
 */
void complexToMagPhase(const double* re, const double* im,
                       double* mag, double* phase, size_t n);

/**
 * @brief Convert magnitude/phase arrays back to complex Re/Im arrays.
 *
 * Computes: re[i] = mag[i] * cos(phase[i])
 *           im[i] = mag[i] * sin(phase[i])
 *
 * In-place operation is supported (mag==re and/or phase==im is valid).
 * Each element is processed independently, so no temporary buffers are needed.
 *
 * @param mag    Input magnitude array
 * @param phase  Input phase array
 * @param re     Output real part array (may be same as mag for in-place)
 * @param im     Output imaginary part array (may be same as phase for in-place)
 * @param n      Number of elements to process
 */
void magPhaseToComplex(const double* mag, const double* phase,
                       double* re, double* im, size_t n);

} // namespace dtcwpt
