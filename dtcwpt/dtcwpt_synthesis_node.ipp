#include "dtcwpt_synthesis_node.h"

#include <cassert>

namespace dtcwpt {

namespace {

/// Taps of `kernel` at indices parity, parity + 2, ... (parity is 0 or 1).
/// StatefulFilter needs a non-empty kernel, so an empty phase (kernel of one
/// tap, odd phase) becomes a single zero tap, which costs no multiplication.
std::vector<double> extractPhase(const std::vector<double>& kernel, size_t parity) {
    std::vector<double> phase;
    for (size_t i = parity; i < kernel.size(); i += 2) {
        phase.push_back(kernel[i]);
    }
    if (phase.empty()) {
        phase.push_back(0.0);
    }
    return phase;
}

} // namespace

SynthesisNode::SynthesisNode(const filters::FilterCoefficients& coefs, bool /*isLevel1*/)
    : filterLowEven_(extractPhase(coefs.g0, 0), coefs.d0)
    , filterLowOdd_(extractPhase(coefs.g0, 1), coefs.d0)
    , filterHighEven_(extractPhase(coefs.g1, 0), coefs.d1)
    , filterHighOdd_(extractPhase(coefs.g1, 1), coefs.d1)
    , delayLow_(coefs.d0)
    , delayHigh_(coefs.d1) {
    assert(!coefs.g0.empty());
    assert(!coefs.g1.empty());
}

void SynthesisNode::synthesizeBlock(const std::vector<double>& lowBuf,
                                    const std::vector<double>& highBuf,
                                    std::vector<double>& outBuf,
                                    size_t outCursor,
                                    size_t numSamples) {
    // Polyphase form of "insert a zero after every sample, then filter with g":
    // the zero-inserted stream is zero at every other position, so an output
    // at an even position only sees the even-index taps and an output at an
    // odd position only sees the odd-index taps, both applied to the subband
    // samples themselves. The terms are summed in the same order as in the
    // zero-insertion form (the omitted terms are all zero), so the result is
    // bit-identical to it.
    for (size_t i = 0; i < numSamples; ++i) {
        filterLowEven_.pushSample(lowBuf[i]);
        filterLowOdd_.pushSample(lowBuf[i]);
        filterHighEven_.pushSample(highBuf[i]);
        filterHighOdd_.pushSample(highBuf[i]);

        outBuf[outCursor + 2 * i] = filterLowEven_.filtering() + filterHighEven_.filtering();
        outBuf[outCursor + 2 * i + 1] = filterLowOdd_.filtering() + filterHighOdd_.filtering();
    }
}

} // namespace dtcwpt
