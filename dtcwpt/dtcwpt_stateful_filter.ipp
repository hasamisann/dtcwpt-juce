#include "dtcwpt_stateful_filter.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace dtcwpt {

StatefulFilter::StatefulFilter(const std::vector<double>& kernel, int delay)
    : kernel_()
    , mirrorBuffer_()
    , size_(1)
    , cursor_(0)
    , readOffset_(0)
    , delay_(delay) {

    assert(!kernel.empty());
    assert(delay >= 0);

    // Strip the leading and trailing zero taps (e.g. the padding tap of the
    // CDF 9/7 representations). A zero coefficient contributes nothing to the
    // sum, so only the span from the first to the last non-zero tap is kept;
    // readOffset_ compensates for the removed leading taps so that the kernel
    // still lines up with the same samples.
    const auto isNonZero = [](double c) { return std::fpclassify(c) != FP_ZERO; };
    const auto first = std::find_if(kernel.begin(), kernel.end(), isNonZero);
    if (first != kernel.end()) {
        const auto last = std::find_if(kernel.rbegin(), kernel.rend(), isNonZero).base();
        kernel_.assign(first, last);
        readOffset_ = static_cast<size_t>(first - kernel.begin());
    }

    // The ring buffer must hold every sample the trimmed kernel can reach.
    size_ = std::max<size_t>(readOffset_ + kernel_.size(), 1);

    // Allocate mirrored buffer (size * 2 for seamless indexing)
    mirrorBuffer_.resize(size_ * 2, 0.0);
}

void StatefulFilter::pushSample(double x) {
    // Write to both cursor_ and cursor_ + size_ (mirrored position).
    // This makes the most-recent size_ samples always available as a contiguous
    // window in the range [cursor_, cursor_ + size_), eliminating the need for
    // modular arithmetic during the convolution in filtering().
    mirrorBuffer_[cursor_] = x;
    mirrorBuffer_[cursor_ + size_] = x;

    // Advance cursor with wrap (a compare instead of an integer division)
    if (++cursor_ == size_) {
        cursor_ = 0;
    }
}

double StatefulFilter::filtering() {
#if DTCWPT_ENABLE_TEST_SEAMS
    macCount_ += kernel_.size();
#endif

    // Read position is offset by size_ into the mirrored buffer to ensure
    // contiguous access to the most-recent size_ samples. The newest sample is
    // at cursor_ - 1 (size_ - 1 when cursor_ is 0) in the primary buffer, i.e.
    // one size_ further in the mirrored buffer. The first kept tap multiplies
    // the sample readOffset_ positions older than the newest one.
    const size_t newest = (cursor_ == 0 ? size_ - 1 : cursor_ - 1) + size_;
    const double* sample = mirrorBuffer_.data() + (newest - readOffset_);

    double result = 0.0;
    for (size_t i = 0; i < kernel_.size(); ++i) {
        result += kernel_[i] * sample[-static_cast<std::ptrdiff_t>(i)];
    }

    return result;
}

void StatefulFilter::reset() {
    std::fill(mirrorBuffer_.begin(), mirrorBuffer_.end(), 0.0);
    cursor_ = 0;
}

} // namespace dtcwpt
