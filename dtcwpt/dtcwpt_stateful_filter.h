#pragma once

#include <cstddef>
#include <vector>

namespace dtcwpt {

/**
 * @brief FIR filter with ring buffer state management.
 *
 * Implements FIR convolution using a mirrored ring buffer technique
 * to eliminate boundary checks during filtering. Maintains internal
 * state for continuous sample-by-sample processing.
 */
class StatefulFilter {
public:
    /**
     * @brief Default constructor (required for vector resize).
     */
    StatefulFilter() = default;

    /**
     * @brief Constructs a StatefulFilter with given kernel and delay.
     *
     * @param kernel Filter coefficients (impulse response)
     * @param delay Filter delay in samples
     */
    StatefulFilter(const std::vector<double>& kernel, int delay);

    /**
     * @brief Pushes a new sample into the ring buffer.
     *
     * Writes the sample to the current cursor position and its
     * mirrored position in the buffer, then advances the cursor.
     *
     * @param x Input sample value
     */
    void pushSample(double x);

    /**
     * @brief Performs FIR convolution and returns filtered output.
     *
     * Computes the convolution of the kernel with the most recent
     * samples in the buffer. Must be called after pushSample().
     *
     * @return Filtered output sample
     */
    double filtering();

    /**
     * @brief Returns the filter delay.
     * @return Delay in samples
     */
    int getDelay() const { return delay_; }

    /**
     * @brief Resets the filter state (ring buffer and cursor) to zero.
     */
    void reset();

#if DTCWPT_ENABLE_TEST_SEAMS
    /**
     * @brief Total multiply-accumulate operations executed by filtering() so far.
     *
     * Test seam: counts every kernel tap visited, including taps whose
     * coefficient or input sample happens to be zero.
     */
    unsigned long long getMacCountForTesting() const noexcept { return macCount_; }

    /// Test seam: clears the multiply-accumulate counter.
    void resetMacCountForTesting() noexcept { macCount_ = 0; }
#endif

private:
    std::vector<double> kernel_;       ///< Filter coefficients
    std::vector<double> mirrorBuffer_; ///< Ring buffer with mirroring (size * 2)
    size_t size_;                      ///< Buffer size (kernel length)
    size_t cursor_;                    ///< Current write position
    int delay_;                        ///< Filter delay
#if DTCWPT_ENABLE_TEST_SEAMS
    unsigned long long macCount_ = 0;  ///< Test seam: multiply-accumulate counter
#endif
};

} // namespace dtcwpt
