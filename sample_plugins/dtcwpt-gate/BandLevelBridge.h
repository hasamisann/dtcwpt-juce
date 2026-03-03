#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <juce_core/juce_core.h>

/**
 * @brief Lock-free audio->GUI transfer of per-band RMS levels.
 *
 * Audio thread writes via update() using try_lock (non-blocking).
 * GUI thread reads via read() (may block briefly).
 * Values are in dB, nominally clamped to [-80, 0] by the caller.
 */
class BandLevelBridge
{
public:
    static constexpr int kMaxBands = 256;

    BandLevelBridge() noexcept { levelsDb_.fill(-80.0f); }

    /** Audio thread: update band levels. Non-blocking (try_lock). */
    void update(const float* levelsDb, int numBands) noexcept
    {
        const int n = std::min(numBands, kMaxBands);
        if (mutex_.try_lock())
        {
            std::copy(levelsDb, levelsDb + n, levelsDb_.begin());
            std::fill(levelsDb_.begin() + n, levelsDb_.end(), -80.0f);
            mutex_.unlock();
        }
        numBands_.store(n, std::memory_order_relaxed);
    }

    /** GUI thread: read current band levels. Returns number of active bands. */
    int read(float* outLevelsDb) const
    {
        const int n = numBands_.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mutex_);
        std::copy(levelsDb_.begin(), levelsDb_.begin() + n, outLevelsDb);
        return n;
    }

private:
    mutable std::mutex mutex_;
    std::array<float, kMaxBands> levelsDb_;
    std::atomic<int> numBands_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BandLevelBridge)
};
