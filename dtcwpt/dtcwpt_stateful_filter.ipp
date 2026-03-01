#include "dtcwpt_stateful_filter.h"

#include <cassert>
#include <cstring>

namespace dtcwpt {

StatefulFilter::StatefulFilter(const std::vector<double>& kernel, int delay)
    : kernel_(kernel)
    , mirrorBuffer_()
    , size_(kernel.size() + 1)
    , cursor_(0)
    , delay_(delay) {
    
    assert(!kernel.empty());
    assert(delay >= 0);
    
    // Allocate mirrored buffer (size * 2 for seamless indexing)
    mirrorBuffer_.resize(size_ * 2, 0.0);
    
    cursor_ = 0;
}

void StatefulFilter::pushSample(double x) {
    // Write to both cursor_ and cursor_ + size_ (mirrored position).
    // This makes the most-recent size_ samples always available as a contiguous
    // window in the range [cursor_, cursor_ + size_), eliminating the need for
    // modular arithmetic during the convolution in filtering().
    mirrorBuffer_[cursor_] = x;
    mirrorBuffer_[cursor_ + size_] = x;

    // Advance cursor with wrap
    cursor_ = (cursor_ + 1) % size_;
}

double StatefulFilter::filtering() {
    // Read position is offset by size_ into the mirrored buffer to ensure
    // contiguous access to the most-recent size_ samples. When cursor_ is 0,
    // the most recent sample is at position size_ - 1 in the primary buffer,
    // which corresponds to size_ - 1 + size_ in the mirrored buffer.
    int idx;
    if (cursor_ == 0) {
        idx = static_cast<int>(size_) - 1 + static_cast<int>(size_);
    } else {
        idx = static_cast<int>(cursor_) - 1 + static_cast<int>(size_);
    }
    
    double result = 0.0;
    for (size_t i = 0; i < kernel_.size(); ++i) {
        result += kernel_[i] * mirrorBuffer_[static_cast<size_t>(idx)];
        idx -= 1;
    }
    
    return result;
}

void StatefulFilter::reset() {
    std::fill(mirrorBuffer_.begin(), mirrorBuffer_.end(), 0.0);
    cursor_ = 0;
}

} // namespace dtcwpt
