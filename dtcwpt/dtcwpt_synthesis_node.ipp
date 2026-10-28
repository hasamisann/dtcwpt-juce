#include "dtcwpt_synthesis_node.h"

namespace dtcwpt {

SynthesisNode::SynthesisNode(const filters::FilterCoefficients& coefs, bool /*isLevel1*/)
    : filterLow_(coefs.g0, coefs.d0)
    , filterHigh_(coefs.g1, coefs.d1) {
}

void SynthesisNode::synthesizeBlock(const std::vector<double>& lowBuf,
                                    const std::vector<double>& highBuf,
                                    std::vector<double>& outBuf,
                                    size_t outCursor,
                                    size_t numSamples) {
    // Upsample by 2:1 (zero insertion) and filter both subbands.
    // Each input sample produces two output samples:
    //   - Even: push actual subband values, write filtered sum
    //   - Odd:  push zeros (upsampling), write filtered sum
    for (size_t i = 0; i < numSamples; ++i) {
        // Even output sample
        outBuf[outCursor + 2 * i] = processTick(lowBuf[i], highBuf[i]);

        // Odd output sample (zero insertion for upsampling)
        outBuf[outCursor + 2 * i + 1] = processTick(0.0, 0.0);
    }
}

double SynthesisNode::processTick(double lowIn, double highIn) {
    filterLow_.pushSample(lowIn);
    filterHigh_.pushSample(highIn);
    return filterLow_.filtering() + filterHigh_.filtering();
}

} // namespace dtcwpt
