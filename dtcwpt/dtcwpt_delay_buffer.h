#pragma once

#include <cstddef>
#include <vector>

namespace dtcwpt {

/**
 * @brief Ring buffer implementation for delay compensation.
 * 
 * Implements delay compensation for DT-CWPT subband alignment.
 * Uses a double-buffered ring buffer with power-of-2 sizing for efficient
 * bit-mask indexing.
 * 
 * The buffer size is the next power of 2 greater than blockSize*2 + delay,
 * with mirrored storage for seamless wrap-around reads.
 */
class DelayBuffer {
public:
    /**
     * @brief Default constructor (required for map operations).
     */
    DelayBuffer() : size_(0), buffer_(), writeCursor_(0), delay_(0) {}

    /**
     * @brief Constructs a DelayBuffer with specified delay and block size.
     * 
     * @param delaySamples The delay in samples (0 for pass-through)
     * @param blockSize The maximum expected block size for processing
     */
    DelayBuffer(int delaySamples, int blockSize);
    
    /**
     * @brief Process a block of samples through the delay buffer.
     * 
     * Reads delayed samples from the buffer and writes new input samples.
     * Supports in-place processing (input and output can be the same buffer).
     * 
     * @param input Input sample block
     * @param output Output sample block (receives delayed samples)
     */
    void process(const std::vector<double>& input, std::vector<double>& output, size_t length);

private:
    size_t size_;               ///< Ring buffer size (power of 2)
    std::vector<double> buffer_; ///< Mirrored ring buffer storage
    size_t writeCursor_;        ///< Current write position
    int delay_;                 ///< Delay in samples
    
    /**
     * @brief Calculate the next power of 2 greater than or equal to n.
     */
    static size_t nextPowerOf2(size_t n);
};

} // namespace dtcwpt
