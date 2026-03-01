#include "dtcwpt_delay_buffer.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace dtcwpt {

DelayBuffer::DelayBuffer(int delaySamples, int blockSize)
    : size_(0)
    , buffer_()
    , writeCursor_(0)
    , delay_(delaySamples) {
    
    assert(delaySamples >= 0);
    assert(blockSize > 0);
    
    // Buffer size must be the smallest power of 2 >= (blockSize * 2 + delaySamples)
    size_t min_size = static_cast<size_t>(blockSize * 2 + delaySamples);
    size_ = nextPowerOf2(min_size);
    
    // Allocate buffer (mirrored - size * 2 for seamless wrap reads)
    buffer_.resize(size_ * 2, 0.0);
    
    writeCursor_ = 0;
}

void DelayBuffer::process(const std::vector<double>& input, std::vector<double>& output, size_t length) {
    const size_t n = length;
    assert(input.size() >= n);
    assert(output.size() >= n);
    
    if (n == 0) {
        return;
    }
    
    // Handle pass-through case (delay = 0)
    if (delay_ == 0) {
        if (input.data() != output.data()) {
            std::copy_n(input.begin(), n, output.begin());
        }
        return;
    }
    
    // 1. Write input to buffer at writeCursor_ (mirrored)
    size_t endPos = writeCursor_ + n;
    if (endPos <= size_) {
        // No wrap-around
        std::copy_n(input.begin(), n, buffer_.begin() + writeCursor_);
        std::copy_n(input.begin(), n, buffer_.begin() + writeCursor_ + size_);
    } else {
        // Wrap-around write
        size_t remain = size_ - writeCursor_;
        std::copy_n(input.begin(), remain, buffer_.begin() + writeCursor_);
        std::copy_n(input.begin(), remain, buffer_.begin() + writeCursor_ + size_);
        std::copy_n(input.begin() + remain, n - remain, buffer_.begin());
        std::copy_n(input.begin() + remain, n - remain, buffer_.begin() + size_);
    }
    
    // 2. Calculate positions and read delayed samples
    const size_t mask = size_ - 1;
    size_t currentHead = (writeCursor_ + n) & mask;
    size_t readPos = (currentHead - n - static_cast<size_t>(delay_)) & mask;
    
    // Read delayed samples
    std::copy(buffer_.begin() + readPos, buffer_.begin() + readPos + n, output.begin());
    
    // Update write cursor
    writeCursor_ = currentHead;
}

size_t DelayBuffer::nextPowerOf2(size_t n) {
    if (n == 0) {
        return 1;
    }
    
    // Decrement n to handle case where n is already a power of 2
    n--;
    
    // Set all bits to the right of the highest set bit
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    
    // For 64-bit size_t
    if constexpr (sizeof(size_t) == 8) {
        n |= n >> 32;
    }
    
    // Increment to get the next power of 2
    return n + 1;
}

} // namespace dtcwpt
