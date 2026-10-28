#pragma once

#include <atomic>

/**
 * Tail length of the committed engine, readable from any thread.
 *
 * The topology is committed on one thread while the host may call getTailLengthSeconds()
 * on another, so the sample count and the sample rate it was computed for are kept in atomics.
 */
class CommittedTailLength
{
public:
    void store (int tailSamples, double sampleRate) noexcept
    {
        sampleRate_.store (sampleRate, std::memory_order_relaxed);
        tailSamples_.store (tailSamples, std::memory_order_relaxed);
    }

    /** Returns 0 while no tail has been stored or the sample rate is not a positive number. */
    double getSeconds() const noexcept
    {
        const int tailSamples = tailSamples_.load (std::memory_order_relaxed);
        const double sampleRate = sampleRate_.load (std::memory_order_relaxed);

        if (tailSamples <= 0 || ! (sampleRate > 0.0))
            return 0.0;

        return static_cast<double> (tailSamples) / sampleRate;
    }

private:
    std::atomic<int> tailSamples_ { 0 };
    std::atomic<double> sampleRate_ { 0.0 };
};
