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

    // Allocate scratch buffers (one-time allocation — zero cost on audio thread)
    const auto bufSize = static_cast<std::size_t> (maxSamplesPerBand);
    mainMag_.assign   (bufSize, 0.0);
    mainPhase_.assign (bufSize, 0.0);
    scMag_.assign     (bufSize, 0.0);
    scPhase_.assign   (bufSize, 0.0);

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

            // Step 1: Convert both inputs to polar form (using scratch buffers)
            dtcwpt::complexToMagPhase (mainView.re, mainView.im,
                                       mainMag_.data(), mainPhase_.data(), n);
            dtcwpt::complexToMagPhase (scView.re, scView.im,
                                       scMag_.data(), scPhase_.data(), n);

            // Step 2–6: Per-sample morph
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

                const double mainM = mainMag_[s];
                const double mainP = mainPhase_[s];
                const double scM   = scMag_[s];
                const double scP   = scPhase_[s];

                // Step 3: Magnitude lerp
                const double outMag = mainM + mag * (scM - mainM);

                // Step 4: Threshold gate
                double effPhase = ph;
                if (scM < thr && thr > 0.0)
                {
                    effPhase *= (scM * 0.5 / thr);
                    effPhase  = std::clamp (effPhase, 0.0, ph);
                }

                // Step 5: Phase N-Lerp via unit-vector interpolation
                const double rx = std::cos (mainP);
                const double ry = std::sin (mainP);
                const double sx = std::cos (scP);
                const double sy = std::sin (scP);
                const double lx = rx + effPhase * (sx - rx);
                const double ly = ry + effPhase * (sy - ry);
                const double outPhase = std::atan2 (ly, lx);

                // Step 6: Convert back to Re/Im (write in-place to main bands)
                mainView.re[s] = outMag * std::cos (outPhase);
                mainView.im[s] = outMag * std::sin (outPhase);
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
