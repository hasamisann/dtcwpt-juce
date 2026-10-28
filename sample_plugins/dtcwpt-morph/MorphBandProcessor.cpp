/**
 * @file MorphBandProcessor.cpp
 * @brief DT-CWPT Morph plugin subband processor implementation.
 */

#include "MorphBandProcessor.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

//==============================================================================
// BandProcessor overrides
//==============================================================================

void MorphBandProcessor::prepare (double sampleRate,
                                   int    maxSamplesPerBand,
                                   int    /*numBands*/,
                                   int    /*numChannels*/)
{
    maxSamplesPerBand_ = maxSamplesPerBand;

    // Allocate scratch buffers (one-time allocation — zero cost on audio thread)
    const auto bufSize = static_cast<std::size_t> (maxSamplesPerBand);
    mainMag_.assign    (bufSize, 0.0);
    mainPhase_.assign  (bufSize, 0.0);
    scMag_.assign      (bufSize, 0.0);
    scPhase_.assign    (bufSize, 0.0);
    magRatio_.assign   (bufSize, 0.0);
    phaseRatio_.assign (bufSize, 0.0);
    threshold_.assign  (bufSize, 0.0);
    active_.assign     (bufSize, 1);

    // 20 ms ramp, defined in host samples
    constexpr double kRampMs = 20.0;
    rampSamples_ = sampleRate * (kRampMs / 1000.0);

    // Snap to the current values immediately (no residual ramp from previous sessions)
    reset();
}

void MorphBandProcessor::setTargets (double magnitude, double phase,
                                      double threshLinear,
                                      bool bypassLow, bool bypassHigh,
                                      std::int64_t streamPosition) noexcept
{
    const Targets requested { magnitude, phase, threshLinear, bypassLow, bypassHigh };

    // Nothing changes if the request repeats the latest one (the usual case: one
    // call per host block with unchanged parameters).
    if (requested == latestTargets_)
        return;

    // Queue full: merge the two oldest events into one that keeps the newer
    // event's position and values, i.e. drop the oldest. The latest requests win.
    if (numEvents_ == kEventQueueCapacity)
    {
        std::copy (events_.begin() + 1, events_.end(), events_.begin());
        --numEvents_;
    }

    // Positions are monotonic
    if (numEvents_ > 0)
        streamPosition = std::max (streamPosition, events_[static_cast<std::size_t> (numEvents_ - 1)].position);

    events_[static_cast<std::size_t> (numEvents_++)] = { streamPosition, requested };
    latestTargets_ = requested;
}

void MorphBandProcessor::processAllBands (dtcwpt::BandData& data)
{
    if (data.destinationIds == nullptr || data.numChannels <= 0)
        return;

    const int numBands    = data.numBands;
    const int numChannels = data.numChannels;

    // Internal block length T in host samples: numSamples * 2^level per band
    // (equal for all bands of a complete leaf set).
    std::int64_t blockLength = 0;
    for (int b = 0; b < numBands; ++b)
    {
        const int nodeId = (*data.destinationIds)[static_cast<std::size_t> (b)];
        const auto n     = static_cast<std::int64_t> (data.bands[0][static_cast<std::size_t> (b)].numSamples);
        blockLength = std::max (blockLength, n << nodeLevel (nodeId));
    }

    if (blockLength == 0)
        return;

    beginBlock (blockLength);

    if (data.hasSidechain)
    {
        for (int b = 0; b < numBands; ++b)
        {
            const int nodeId = (*data.destinationIds)[static_cast<std::size_t> (b)];
            const bool isLow  = isLowBand  (nodeId);
            const bool isHigh = isHighBand (nodeId);
            const double bandScale = static_cast<double> (std::int64_t { 1 } << nodeLevel (nodeId));

            // Step 0: parameter values per subband sample, shared by all channels.
            // Sample s sits at time tau = (s + 0.5) * 2^level inside the block.
            const std::size_t bandSamples = std::min (data.bands[0][static_cast<std::size_t> (b)].numSamples,
                                                      active_.size());
            bool anyActive = false;
            int  seg = 0;
            for (std::size_t s = 0; s < bandSamples; ++s)
            {
                const double tau = (static_cast<double> (s) + 0.5) * bandScale;
                while (seg + 1 < numSegments_ && tau >= segments_[static_cast<std::size_t> (seg + 1)].tau)
                    ++seg;

                const Segment& sg = segments_[static_cast<std::size_t> (seg)];
                const double t = tau - sg.tau;
                magRatio_[s]   = sg.magnitude.value (t);
                phaseRatio_[s] = sg.phase.value (t);
                threshold_[s]  = sg.threshold.value (t);

                const bool bypassed = (sg.bypassLow && isLow) || (sg.bypassHigh && isHigh);
                active_[s] = bypassed ? 0 : 1;
                anyActive = anyActive || ! bypassed;
            }

            if (! anyActive)
                continue;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto& mainView = data.bands
                                           [static_cast<std::size_t> (ch)]
                                           [static_cast<std::size_t> (b)];
                const auto& scView   = data.sidechainBands
                                           [static_cast<std::size_t> (ch)]
                                           [static_cast<std::size_t> (b)];

                const std::size_t n = std::min (mainView.numSamples, bandSamples);

                // Step 1: Convert both inputs to polar form (using scratch buffers)
                dtcwpt::complexToMagPhase (mainView.re, mainView.im,
                                           mainMag_.data(), mainPhase_.data(), n);
                dtcwpt::complexToMagPhase (scView.re, scView.im,
                                           scMag_.data(), scPhase_.data(), n);

                // Step 2–6: Per-sample morph
                for (std::size_t s = 0; s < n; ++s)
                {
                    if (! active_[s])
                        continue;

                    const double mag = magRatio_[s];
                    const double ph  = phaseRatio_[s];
                    const double thr = threshold_[s];

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

    endBlock (blockLength);
}

void MorphBandProcessor::reset()
{
    // Snap all ramps to the latest requested targets, drop pending events and
    // restart the stream timeline.
    numEvents_ = 0;
    state_ = Segment {};
    state_.magnitude.setCurrentAndTarget (latestTargets_.magnitude);
    state_.phase.setCurrentAndTarget     (latestTargets_.phase);
    state_.threshold.setCurrentAndTarget (latestTargets_.threshold);
    state_.bypassLow  = latestTargets_.bypassLow;
    state_.bypassHigh = latestTargets_.bypassHigh;
    blockStart_ = 0;
}

//==============================================================================
// Time-axis helpers
//==============================================================================

void MorphBandProcessor::applyTargets (Segment& seg, const Targets& t) const noexcept
{
    seg.magnitude.retarget (t.magnitude, rampSamples_);
    seg.phase.retarget     (t.phase,     rampSamples_);
    seg.threshold.retarget (t.threshold, rampSamples_);
    seg.bypassLow  = t.bypassLow;
    seg.bypassHigh = t.bypassHigh;
}

void MorphBandProcessor::beginBlock (std::int64_t blockLength) noexcept
{
    const double blockLengthD = static_cast<double> (blockLength);
    segments_[0] = state_;
    segments_[0].tau = 0.0;
    numSegments_ = 1;

    // Events inside this block start a new segment at their time. An event at or
    // before the block start (stale or late) applies from the block start.
    const std::int64_t blockEnd = blockStart_ + blockLength;
    int consumed = 0;
    while (consumed < numEvents_ && events_[static_cast<std::size_t> (consumed)].position < blockEnd)
    {
        const TargetEvent& e = events_[static_cast<std::size_t> (consumed)];
        const double tau = std::clamp (static_cast<double> (e.position - blockStart_), 0.0, blockLengthD);

        Segment next = segments_[static_cast<std::size_t> (numSegments_ - 1)];
        const double dt = tau - next.tau;
        next.magnitude.advance (dt);
        next.phase.advance     (dt);
        next.threshold.advance (dt);
        next.tau = tau;
        applyTargets (next, e.targets);

        segments_[static_cast<std::size_t> (numSegments_++)] = next;
        ++consumed;
    }

    std::copy (events_.begin() + consumed, events_.begin() + numEvents_, events_.begin());
    numEvents_ -= consumed;
}

void MorphBandProcessor::endBlock (std::int64_t blockLength) noexcept
{
    state_ = segments_[static_cast<std::size_t> (numSegments_ - 1)];
    const double dt = static_cast<double> (blockLength) - state_.tau;
    state_.magnitude.advance (dt);
    state_.phase.advance     (dt);
    state_.threshold.advance (dt);
    state_.tau = 0.0;
    blockStart_ += blockLength;
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

int MorphBandProcessor::nodeLevel (int nodeId) noexcept
{
    // floor(log2(nodeId)): the root (ID 1) is level 0
    int level = 0;
    while (nodeId > 1)
    {
        nodeId >>= 1;
        ++level;
    }
    return level;
}
