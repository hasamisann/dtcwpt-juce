/**
 * @file MorphBandProcessor.cpp
 * @brief DT-CWPT Morph plugin subband processor implementation.
 */

#include "MorphBandProcessor.h"

#include <algorithm>
#include <cassert>
#include <cmath>

//==============================================================================
// BandProcessor overrides
//==============================================================================

void MorphBandProcessor::prepare (double sampleRate,
                                   int    maxSamplesPerBand,
                                   int    /*numBands*/,
                                   int    /*numChannels*/)
{
    sampleRate_        = sampleRate;
    maxSamplesPerBand_ = maxSamplesPerBand;

    // Initialise SmoothedValues with 20 ms ramp time
    constexpr double kRampMs = 20.0;
    const double rampSamples = sampleRate * (kRampMs / 1000.0);

    magnitudeSmoothed_.reset  (sampleRate, kRampMs / 1000.0);
    phaseSmoothed_.reset      (sampleRate, kRampMs / 1000.0);
    thresholdSmoothed_.reset  (sampleRate, kRampMs / 1000.0);

    juce::ignoreUnused (rampSamples); // rampSamples used only for documentation

    // Snap to current values immediately (no residual ramp from previous sessions)
    magnitudeSmoothed_.setCurrentAndTargetValue  (magnitudeSmoothed_.getTargetValue());
    phaseSmoothed_.setCurrentAndTargetValue      (phaseSmoothed_.getTargetValue());
    thresholdSmoothed_.setCurrentAndTargetValue  (thresholdSmoothed_.getTargetValue());
}

void MorphBandProcessor::setTargets (double magnitude, double phase,
                                      double threshLinear,
                                      bool bypassLow, bool bypassHigh) noexcept
{
    magnitudeSmoothed_.setTargetValue  (magnitude);
    phaseSmoothed_.setTargetValue      (phase);
    thresholdSmoothed_.setTargetValue  (threshLinear);
    bypassLow_  = bypassLow;
    bypassHigh_ = bypassHigh;
}

void MorphBandProcessor::processAllBands (dtcwpt::BandData& data)
{
    if (! data.hasSidechain || data.destinationIds == nullptr)
        return;

    const int numBands    = data.numBands;
    const int numChannels = data.numChannels;

    for (int b = 0; b < numBands; ++b)
    {
        const int nodeId = (*data.destinationIds)[static_cast<std::size_t> (b)];

        // Bypass low/high bands if requested
        if (bypassLow_  && isLowBand  (nodeId)) continue;
        if (bypassHigh_ && isHighBand (nodeId)) continue;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto& mainView = data.bands
                                       [static_cast<std::size_t> (ch)]
                                       [static_cast<std::size_t> (b)];
            const auto& scView   = data.sidechainBands
                                       [static_cast<std::size_t> (ch)]
                                       [static_cast<std::size_t> (b)];

            const std::size_t n = mainView.numSamples;

            // Per-sample morph
            const bool isFirstChannel = (ch == 0);

            for (std::size_t s = 0; s < n; ++s)
            {
                // Advance SmoothedValues only on first channel
                double mag, ph, thr;
                if (isFirstChannel)
                {
                    mag = magnitudeSmoothed_.getNextValue();
                    ph  = phaseSmoothed_.getNextValue();
                    thr = thresholdSmoothed_.getNextValue();
                }
                else
                {
                    mag = magnitudeSmoothed_.getCurrentValue();
                    ph  = phaseSmoothed_.getCurrentValue();
                    thr = thresholdSmoothed_.getCurrentValue();
                }

                // Step 1: Magnitudes and unit direction vectors of both inputs.
                // The direction (re/|c|, im/|c|) equals (cos(arg c), sin(arg c))
                // without any trigonometry. A zero vector has no direction; it is
                // defined as (1, 0), i.e. phase 0 (the value atan2(0, 0) returns).
                const double mainRe = mainView.re[s];
                const double mainIm = mainView.im[s];
                const double scRe   = scView.re[s];
                const double scIm   = scView.im[s];

                const double mainM = std::sqrt (mainRe * mainRe + mainIm * mainIm);
                const double scM   = std::sqrt (scRe * scRe + scIm * scIm);

                double rx = 1.0, ry = 0.0;
                if (mainM > 0.0)
                {
                    const double inv = 1.0 / mainM;
                    rx = mainRe * inv;
                    ry = mainIm * inv;
                }

                double sx = 1.0, sy = 0.0;
                if (scM > 0.0)
                {
                    const double inv = 1.0 / scM;
                    sx = scRe * inv;
                    sy = scIm * inv;
                }

                // Step 2: Magnitude lerp
                const double outMag = mainM + mag * (scM - mainM);

                // Step 3: Threshold gate
                double effPhase = ph;
                if (scM < thr && thr > 0.0)
                {
                    effPhase *= (scM * 0.5 / thr);
                    effPhase  = std::clamp (effPhase, 0.0, ph);
                }

                // Step 4: Phase N-Lerp via unit-vector interpolation. The lerped
                // vector is normalised back to a unit direction; if it cancels to
                // zero length the direction is again defined as (1, 0).
                const double lx = rx + effPhase * (sx - rx);
                const double ly = ry + effPhase * (sy - ry);
                const double lenSq = lx * lx + ly * ly;

                double ux = 1.0, uy = 0.0;
                if (lenSq > 0.0)
                {
                    const double inv = 1.0 / std::sqrt (lenSq);
                    ux = lx * inv;
                    uy = ly * inv;
                }

                // Step 5: Write back to Re/Im (in-place to main bands)
                mainView.re[s] = outMag * ux;
                mainView.im[s] = outMag * uy;
            }
        }
    }
}

void MorphBandProcessor::reset()
{
    // Snap SmoothedValues to their current target values (no residual ramp)
    magnitudeSmoothed_.setCurrentAndTargetValue  (magnitudeSmoothed_.getTargetValue());
    phaseSmoothed_.setCurrentAndTargetValue      (phaseSmoothed_.getTargetValue());
    thresholdSmoothed_.setCurrentAndTargetValue  (thresholdSmoothed_.getTargetValue());
}

//==============================================================================
// Private helpers
//==============================================================================

bool MorphBandProcessor::isLowBand (int nodeId) noexcept
{
    // Power of 2: exactly one bit set — represents the L-only deep path
    return nodeId > 0 && (nodeId & (nodeId - 1)) == 0;
}

bool MorphBandProcessor::isHighBand (int nodeId) noexcept
{
    // All bits set: (n & (n+1)) == 0 — represents the H-only deep path
    return nodeId > 0 && (nodeId & (nodeId + 1)) == 0;
}
