#include "GateBandProcessor.h"
#include <cmath>
#include <algorithm>
#include <bit>

namespace
{
    /** Tree depth of a destination node (root = 1 -> level 0; a node at depth d is decimated by 2^d). */
    int nodeLevel(int nodeId) noexcept
    {
        return nodeId > 0 ? std::min(static_cast<int>(std::bit_width(static_cast<unsigned>(nodeId))) - 1, 30) : 0;
    }

    /** One-pole smoothing coefficient 1 - exp(-1 / (tau * fs)); 1 (no smoothing) for tau <= 0. */
    double smoothingCoefficient(double timeMs, double rate) noexcept
    {
        const double samples = timeMs * 1.0e-3 * rate;
        return samples > 0.0 ? 1.0 - std::exp(-1.0 / samples) : 1.0;
    }

    int msToSamples(double timeMs, double rate) noexcept
    {
        return static_cast<int>(std::lround(std::max(0.0, timeMs) * 1.0e-3 * rate));
    }
}

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

    const double closedGain = std::pow(10.0, std::min(dynamics_.rangeDb, 0.0) / 20.0);
    const double closeFactor = std::pow(10.0, -std::max(dynamics_.hysteresisDb, 0.0) / 20.0);

    for (int b = 0; b < numBands_; ++b)
    {
        if (b >= data.numBands) break;
        double sumSq = 0.0;
        size_t totalSamples = 0;
        const bool shouldBypass = data.destinationIds != nullptr && bypassLow_ && isLowBand((*data.destinationIds)[static_cast<size_t>(b)]);

        // Channels that carry data for this band (the gate state exists for kMaxChannels only)
        dtcwpt::ChannelBandView* views[kMaxChannels] = {};
        int numActive = 0;
        size_t maxSamples = 0;

        for (int ch = 0; ch < data.numChannels; ++ch)
        {
            if (static_cast<size_t>(ch) >= data.bands.size() || static_cast<size_t>(b) >= data.bands[static_cast<size_t>(ch)].size())
                continue;

            auto& view = data.bands[static_cast<size_t>(ch)][static_cast<size_t>(b)];
            for (size_t i = 0; i < view.numSamples; ++i)
                sumSq += view.re[i] * view.re[i] + view.im[i] * view.im[i];
            totalSamples += view.numSamples;

            if (ch < kMaxChannels)
            {
                views[ch] = &view;
                maxSamples = std::max(maxSamples, view.numSamples);
                numActive = ch + 1;
            }
        }

        if (!shouldBypass && numActive > 0)
        {
            // Subband sample rate: depth `level` means decimation by 2^level
            const int level = data.destinationIds != nullptr ? nodeLevel((*data.destinationIds)[static_cast<size_t>(b)]) : 0;
            const double rate = sampleRate_ / static_cast<double>(1u << level);

            const double attackCoef = smoothingCoefficient(dynamics_.attackMs, rate);
            const double releaseCoef = smoothingCoefficient(dynamics_.releaseMs, rate);
            const int holdSamples = msToSamples(dynamics_.holdMs, rate);
            const int rampSamples = std::max(1, msToSamples(kThresholdSmoothingMs, rate));
            const bool linked = dynamics_.stereoLink && numActive == kMaxChannels
                                && views[0] != nullptr && views[1] != nullptr;

            BandState& band = states_[static_cast<size_t>(b)];
            const double goal = thresholds_[static_cast<size_t>(b)];
            if (!band.thresholdInitialised)
            {
                band.thresholdInitialised = true;
                band.thresholdCurrent = band.thresholdGoal = goal;
                band.rampRemaining = 0;
            }
            else if (goal != band.thresholdGoal)
            {
                band.thresholdGoal = goal;
                band.rampRemaining = rampSamples;
                band.thresholdStep = (goal - band.thresholdCurrent) / static_cast<double>(rampSamples);
            }

            // Open/close decision with hysteresis and hold for one detector (a channel, or the linked pair)
            auto decide = [holdSamples](ChannelState& s, double detected, double openThr, double closeThr) noexcept
            {
                if (!s.open)
                {
                    if (detected >= openThr)
                    {
                        s.open = true;
                        s.holdRemaining = holdSamples;
                    }
                }
                else if (detected >= closeThr)
                    s.holdRemaining = holdSamples;
                else if (s.holdRemaining > 0)
                    --s.holdRemaining;
                else
                    s.open = false;
            };

            for (size_t i = 0; i < maxSamples; ++i)
            {
                if (band.rampRemaining > 0)
                {
                    band.thresholdCurrent = --band.rampRemaining == 0 ? band.thresholdGoal
                                                                      : band.thresholdCurrent + band.thresholdStep;
                }
                const double openThr = band.thresholdCurrent;
                const double closeThr = openThr * closeFactor;

                // Envelope followers: rise with attack, fall with release
                for (int ch = 0; ch < numActive; ++ch)
                {
                    if (views[ch] == nullptr || i >= views[ch]->numSamples) continue;
                    ChannelState& s = band.channels[static_cast<size_t>(ch)];
                    const double mag = std::sqrt(views[ch]->re[i] * views[ch]->re[i] + views[ch]->im[i] * views[ch]->im[i]);
                    if (!s.primed)
                    {
                        s.primed = true;
                        s.gain = closedGain;
                    }
                    const double envCoef = mag > s.envelope ? attackCoef : releaseCoef;
                    s.envelope = envCoef >= 1.0 ? mag : s.envelope + envCoef * (mag - s.envelope);
                }

                if (linked)
                {
                    ChannelState& s0 = band.channels[0];
                    ChannelState& s1 = band.channels[1];
                    decide(s0, std::max(s0.envelope, s1.envelope), openThr, closeThr);
                    s1.open = s0.open;
                    s1.holdRemaining = s0.holdRemaining;
                }
                else
                {
                    for (int ch = 0; ch < numActive; ++ch)
                        if (views[ch] != nullptr && i < views[ch]->numSamples)
                        {
                            ChannelState& s = band.channels[static_cast<size_t>(ch)];
                            decide(s, s.envelope, openThr, closeThr);
                        }
                }

                // Gain smoothing towards 1 (open) or the range gain (closed), then apply
                for (int ch = 0; ch < numActive; ++ch)
                {
                    if (views[ch] == nullptr || i >= views[ch]->numSamples) continue;
                    ChannelState& s = band.channels[static_cast<size_t>(ch)];
                    const double target = s.open ? 1.0 : closedGain;
                    const double gainCoef = target > s.gain ? attackCoef : releaseCoef;
                    s.gain += gainCoef * (target - s.gain);
                    if (gainCoef >= 1.0 || std::abs(target - s.gain) < 1.0e-12)
                        s.gain = target;

                    views[ch]->re[i] *= s.gain;
                    views[ch]->im[i] *= s.gain;
                }
            }
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
    states_.fill(BandState {});
}

void GateBandProcessor::setDynamics(const DynamicsParams& params) noexcept
{
    dynamics_ = params;
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
