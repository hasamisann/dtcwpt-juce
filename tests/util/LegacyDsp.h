/**
 * @file LegacyDsp.h
 * @brief Reference implementations of the pre-optimisation StatefulFilter and
 *        SynthesisNode (issue #18), kept in the tests as a bit-exactness oracle.
 *
 * These are verbatim copies of the straightforward implementations: the
 * convolution walks every kernel tap, and synthesis pushes the zero-inserted
 * stream through the full kernels. The optimised engine must reproduce their
 * output bit for bit.
 */

#pragma once

#include <cstddef>
#include <vector>

namespace LegacyDsp {

class StatefulFilter {
public:
    StatefulFilter() = default;

    explicit StatefulFilter(const std::vector<double>& kernel)
        : kernel_(kernel)
        , mirrorBuffer_(2 * (kernel.size() + 1), 0.0)
        , size_(kernel.size() + 1)
        , cursor_(0) {}

    void pushSample(double x) {
        mirrorBuffer_[cursor_] = x;
        mirrorBuffer_[cursor_ + size_] = x;
        cursor_ = (cursor_ + 1) % size_;
    }

    double filtering() const {
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

private:
    std::vector<double> kernel_;
    std::vector<double> mirrorBuffer_;
    size_t size_ = 1;
    size_t cursor_ = 0;
};

/// Zero-insertion synthesis through the full g0 / g1 kernels.
class SynthesisNode {
public:
    SynthesisNode(const std::vector<double>& g0, const std::vector<double>& g1)
        : filterLow_(g0), filterHigh_(g1) {}

    void synthesizeBlock(const double* lowBuf, const double* highBuf,
                         double* outBuf, size_t numSamples) {
        for (size_t i = 0; i < numSamples; ++i) {
            filterLow_.pushSample(lowBuf[i]);
            filterHigh_.pushSample(highBuf[i]);
            outBuf[2 * i] = filterLow_.filtering() + filterHigh_.filtering();

            filterLow_.pushSample(0.0);
            filterHigh_.pushSample(0.0);
            outBuf[2 * i + 1] = filterLow_.filtering() + filterHigh_.filtering();
        }
    }

private:
    StatefulFilter filterLow_;
    StatefulFilter filterHigh_;
};

} // namespace LegacyDsp
