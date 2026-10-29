#pragma once

/**
 * @file LatencyBypass.h
 * @brief Latency-compensated bypass shared by the Morph and Gate plugins.
 *
 * The engine reports a latency to the host (PDC). A bypass that simply passes the
 * input through would move the audio earlier than the host expects, so the bypassed
 * signal has to be delayed by exactly the reported latency. LatencyBypass keeps a
 * per-channel delay line fed with the plug-in input and cross-fades between the
 * engine output ("wet") and the delayed input ("dry") with a linear ramp.
 *
 * Real-time rules:
 *  - All memory is allocated in prepare(). The delay line is sized for the largest
 *    latency the engine can ever report (see maxEngineLatencySamples()), so a
 *    topology change only moves the read offset and never allocates.
 *  - The delay line is fed every block, including while bypassed, and the engine
 *    keeps running as well. Both stay "warm", so leaving bypass neither replays
 *    stale engine state nor exposes an empty delay line.
 */

#include <dtcwpt/dtcwpt_filter_coeffs.h>

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace latency_bypass
{
    /**
     * @brief Upper bound of DTCWPTProcessor::getLatency() for any topology up to maxDepth.
     *
     * The streaming engine latency is the largest synthesis path delay. A path
     * costs the root delay once plus nodeDelay * (2^depth - 2). It has no host
     * block buffering latency. maxBlockSize is retained for source compatibility.
     */
    inline int maxEngineLatencySamples (int maxDepth, int maxBlockSize) noexcept
    {
        namespace filters = dtcwpt::filters;

        const int rootDelay = std::max ({ filters::CDF_RE.d0, filters::CDF_RE.d1,
                                          filters::CDF_IM.d0, filters::CDF_IM.d1 });
        const int nodeDelay = std::max ({ filters::QSHIFT14_RE.d0, filters::QSHIFT14_RE.d1,
                                          filters::QSHIFT14_IM.d0, filters::QSHIFT14_IM.d1,
                                          filters::PACKET.d0, filters::PACKET.d1 });

        const int levels = 1 << std::max (maxDepth, 1);
        const int pathDelay = rootDelay + nodeDelay * (levels - 2);
        (void) maxBlockSize;
        return pathDelay;
    }
} // namespace latency_bypass

class LatencyBypass
{
public:
    /** Fade time between the live and the bypassed signal. */
    static constexpr double kFadeSeconds = 0.010;

    /**
     * @brief Allocates the delay line and scratch buffer. Not real-time safe.
     *
     * @param maxDelaySamples Largest delay that setDelaySamples() will ever be given.
     */
    void prepare (double sampleRate, int maxBlockSize, int numChannels, int maxDelaySamples)
    {
        numChannels_ = std::max (numChannels, 1);
        maxDelaySamples_ = std::max (maxDelaySamples, 0);
        ringSize_ = maxDelaySamples_ + 1;

        rings_.assign (static_cast<std::size_t> (numChannels_),
                       std::vector<double> (static_cast<std::size_t> (ringSize_), 0.0));
        dryBuffer_.setSize (numChannels_, std::max (maxBlockSize, 1));
        dryBuffer_.clear();

        writeIndex_ = 0;
        delaySamples_ = 0;

        gain_.reset (sampleRate, kFadeSeconds);
        gain_.setCurrentAndTargetValue (0.0);
        bypassTarget_ = false;
        started_ = false;
    }

    /** Frees all memory. */
    void release()
    {
        rings_.clear();
        rings_.shrink_to_fit();
        dryBuffer_.setSize (0, 0);
        maxDelaySamples_ = 0;
        ringSize_ = 1;
        writeIndex_ = 0;
        delaySamples_ = 0;
        bypassTarget_ = false;
        started_ = false;
    }

    /** Clears the delay line (silence) and snaps the fade to the current target. */
    void reset() noexcept
    {
        for (auto& ring : rings_)
            std::fill (ring.begin(), ring.end(), 0.0);

        writeIndex_ = 0;
        gain_.setCurrentAndTargetValue (gain_.getTargetValue());
        started_ = false;
    }

    /**
     * @brief Sets the dry delay (the latency reported to the host). Real-time safe.
     *
     * Only the read offset moves. The delay line keeps its history, so a change in latency
     * does not insert silence into the bypassed signal.
     */
    void setDelaySamples (int delaySamples) noexcept
    {
        jassert (rings_.empty() || delaySamples <= maxDelaySamples_);
        delaySamples_ = std::clamp (delaySamples, 0, maxDelaySamples_);
    }

    int getDelaySamples() const noexcept { return delaySamples_; }
    int getCapacitySamples() const noexcept { return maxDelaySamples_; }

    /**
     * @brief Selects the live (false) or bypassed (true) signal. Real-time safe.
     *
     * Before the first block after prepare()/reset() there is no earlier output to stay
     * continuous with, so the state is applied immediately; afterwards it is cross-faded.
     */
    void setBypassed (bool shouldBypass) noexcept
    {
        const double target = shouldBypass ? 1.0 : 0.0;

        if (! started_)
            gain_.setCurrentAndTargetValue (target);
        else if (shouldBypass != bypassTarget_)
            gain_.setTargetValue (target);

        bypassTarget_ = shouldBypass;
    }

    /**
     * @brief Records the plug-in input and produces its delayed copy. Real-time safe.
     *
     * Call before the engine processes (and overwrites) the buffer.
     */
    void captureDry (const juce::AudioBuffer<double>& input) noexcept
    {
        const int numSamples = input.getNumSamples();
        const int channels = std::min (numChannels_, input.getNumChannels());

        if (rings_.empty() || numSamples <= 0)
            return;

        // Same convention as the processors' own buffers: only grows if the host exceeds
        // the prepared block size.
        dryBuffer_.setSize (numChannels_, numSamples, false, false, true);

        for (int ch = 0; ch < numChannels_; ++ch)
        {
            auto& ring = rings_[static_cast<std::size_t> (ch)];
            const double* src = input.getReadPointer (std::min (ch, channels - 1));
            double* dry = dryBuffer_.getWritePointer (ch);
            int write = writeIndex_;

            for (int i = 0; i < numSamples; ++i)
            {
                ring[static_cast<std::size_t> (write)] = src[i];

                int read = write - delaySamples_;
                if (read < 0)
                    read += ringSize_;

                dry[i] = ring[static_cast<std::size_t> (read)];

                if (++write >= ringSize_)
                    write = 0;
            }
        }

        writeIndex_ = (writeIndex_ + numSamples) % ringSize_;
    }

    /**
     * @brief Cross-fades the engine output in `wet` with the dry signal from captureDry().
     *
     * Fully live leaves `wet` untouched; fully bypassed writes the delayed input verbatim.
     */
    void mixInto (juce::AudioBuffer<double>& wet) noexcept
    {
        started_ = true;

        const int numSamples = std::min (wet.getNumSamples(), dryBuffer_.getNumSamples());
        const int channels = std::min (numChannels_, wet.getNumChannels());

        if (numSamples <= 0)
            return;

        if (! gain_.isSmoothing())
        {
            if (gain_.getTargetValue() <= 0.0)
                return;

            for (int ch = 0; ch < channels; ++ch)
                std::copy_n (dryBuffer_.getReadPointer (ch), numSamples, wet.getWritePointer (ch));
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const double dryGain = gain_.getNextValue();
            const double wetGain = 1.0 - dryGain;

            for (int ch = 0; ch < channels; ++ch)
            {
                double* out = wet.getWritePointer (ch);
                out[i] = out[i] * wetGain + dryBuffer_.getReadPointer (ch)[i] * dryGain;
            }
        }
    }

private:
    std::vector<std::vector<double>> rings_;
    juce::AudioBuffer<double> dryBuffer_;
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear> gain_ { 0.0 };

    int numChannels_ { 2 };
    int maxDelaySamples_ { 0 };
    int ringSize_ { 1 };
    int writeIndex_ { 0 };
    int delaySamples_ { 0 };
    bool bypassTarget_ { false };
    bool started_ { false };
};
