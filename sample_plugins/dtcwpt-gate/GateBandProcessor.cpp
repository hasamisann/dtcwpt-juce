#include "GateBandProcessor.h"
#include <cmath>
#include <algorithm>

GateBandProcessor::GateBandProcessor() noexcept
{
    thresholds_.fill(0.0);
    bandLevelsDb_.fill(-80.0f);
}

void GateBandProcessor::prepare(double sampleRate, int maxSamplesPerBand, int numBands, int numChannels)
{
    sampleRate_ = sampleRate;
    maxSamplesPerBand_ = maxSamplesPerBand;
    numBands_ = std::min(numBands, kMaxBands);
    numChannels_ = numChannels;
    
    reset();
}

void GateBandProcessor::processAllBands(dtcwpt::BandData& data) noexcept
{
    if (data.numChannels <= 0 || data.numBands <= 0) return;

    for (int b = 0; b < numBands_; ++b)
    {
        if (b >= data.numBands) break;
        double sumSq = 0.0;
        size_t totalSamples = 0;
        const double threshold = thresholds_[static_cast<size_t>(b)];

        for (int ch = 0; ch < data.numChannels; ++ch)
        {
            auto& view = data.bands[static_cast<size_t>(ch)][static_cast<size_t>(b)];
            double* re = view.re;
            double* im = view.im;
            const size_t numSamples = view.numSamples;

            for (size_t i = 0; i < numSamples; ++i)
            {
                const double r = re[i];
                const double i_ = im[i];
                const double sqMag = r * r + i_ * i_;
                sumSq += sqMag;

                if (std::sqrt(sqMag) < threshold)
                {
                    re[i] = 0.0;
                    im[i] = 0.0;
                }
            }
            totalSamples += numSamples;
        }

        if (totalSamples > 0)
        {
            const double meanSq = sumSq / static_cast<double>(totalSamples);
            const double rms = std::sqrt(meanSq);
            const float db = static_cast<float>(20.0 * std::log10(rms + 1e-10));
            bandLevelsDb_[static_cast<size_t>(b)] = std::clamp(db, -80.0f, 0.0f);
        }
    }
}

void GateBandProcessor::reset()
{
    bandLevelsDb_.fill(-80.0f);
}

void GateBandProcessor::setThresholds(const double* thresholds, int numBands) noexcept
{
    numBands_ = std::min(numBands, kMaxBands);
    for (int i = 0; i < numBands_; ++i)
    {
        thresholds_[static_cast<size_t>(i)] = thresholds[i];
    }
}

int GateBandProcessor::getBandLevelsDb(float* outLevelsDb) const noexcept
{
    for (int i = 0; i < numBands_; ++i)
    {
        outLevelsDb[i] = bandLevelsDb_[static_cast<size_t>(i)];
    }
    return numBands_;
}
