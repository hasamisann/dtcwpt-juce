#pragma once

#include <atomic>

/** Coherent tail duration of the committed engine, readable from any thread. */
class CommittedTailLength
{
public:
    void store (int tailSamples, double sampleRate) noexcept
    {
        const double seconds = tailSamples > 0 && sampleRate > 0.0
                             ? static_cast<double> (tailSamples) / sampleRate : 0.0;
        seconds_.store (seconds, std::memory_order_relaxed);
    }

    double getSeconds() const noexcept { return seconds_.load (std::memory_order_relaxed); }

private:
    std::atomic<double> seconds_ { 0.0 };
};
