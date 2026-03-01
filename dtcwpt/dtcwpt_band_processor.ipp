/**
 * @file BandProcessor.cpp
 * @brief Implementation of BandProcessor default methods and LambdaBandProcessor.
 */

#include "dtcwpt_band_processor.h"

namespace dtcwpt {

void BandProcessor::processAllBands(BandData& data) {
    for (int ch = 0; ch < data.numChannels; ++ch) {
        for (int b = 0; b < data.numBands; ++b) {
            auto& view = data.bands[ch][b];
            processBand(b, view.re, view.im, view.numSamples);
        }
    }
}

void BandProcessor::processBand(int /*bandIndex*/, double* /*re*/,
                                double* /*im*/, size_t /*numSamples*/) {
    // No-op default — pass-through
}

// Internal LambdaBandProcessor implementation (not exposed in header)
namespace {

class LambdaBandProcessor : public BandProcessor {
public:
    explicit LambdaBandProcessor(BandProcessorFunc func) : func_(std::move(func)) {}

    void prepare(double /*sampleRate*/, int /*maxSamplesPerBand*/,
                 int /*numBands*/, int /*numChannels*/) override {
        // No-op
    }

    void reset() override {
        // No-op
    }

    void processBand(int bandIndex, double* re, double* im, size_t numSamples) override {
        func_(bandIndex, re, im, numSamples);
    }

private:
    BandProcessorFunc func_;
};

} // anonymous namespace

std::unique_ptr<BandProcessor> makeLambdaProcessor(BandProcessorFunc func) {
    return std::make_unique<LambdaBandProcessor>(std::move(func));
}

} // namespace dtcwpt
