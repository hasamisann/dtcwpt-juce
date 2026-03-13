#include "dtcwpt_analysis_node.h"

namespace dtcwpt {

AnalysisNode::AnalysisNode(const filters::FilterCoefficients& coefs, bool /*isLevel1*/)
    : filterLow_(coefs.h0, coefs.d0)
    , filterHigh_(coefs.h1, coefs.d1)
    , parity_(false) {
}

bool AnalysisNode::updateBuffer(double x,
                                std::vector<double>& workBuffer,
                                std::vector<char>& activeFlags,
                                int lowIdx,
                                int highIdx) {
    // Push sample to both filters
    filterLow_.pushSample(x);
    filterHigh_.pushSample(x);

    // Check parity for 2:1 downsampling
    if (parity_) {
        // Odd sample: skip calculation, toggle parity
        parity_ = false;
        return false;
    } else {
        // Even sample: calculate and write outputs
        workBuffer[static_cast<size_t>(lowIdx)] = filterLow_.filtering();
        workBuffer[static_cast<size_t>(highIdx)] = filterHigh_.filtering();
        activeFlags[static_cast<size_t>(lowIdx)] = 1;
        activeFlags[static_cast<size_t>(highIdx)] = 1;
        parity_ = true;
        return true;
    }
}

bool AnalysisNode::processArrival(double sampleValue, AnalysisChildOutputs& outputs) noexcept {
    // Push sample to both filters
    filterLow_.pushSample(sampleValue);
    filterHigh_.pushSample(sampleValue);

    // Check parity for 2:1 downsampling
    if (parity_) {
        // Odd sample: skip calculation, toggle parity
        parity_ = false;
        return false;
    } else {
        // Even sample: calculate and write outputs
        outputs.low = filterLow_.filtering();
        outputs.high = filterHigh_.filtering();
        parity_ = true;
        return true;
    }
}

} // namespace dtcwpt
