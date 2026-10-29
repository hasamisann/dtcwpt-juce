#pragma once

#include "../common/TripleBuffer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

/**
 * @brief Lock-free audio->GUI transfer of per-band RMS levels.
 *
 * Built on `TripleBuffer`: the band count and the levels travel together in
 * one snapshot, so the GUI never combines a band count with the levels of a
 * different update. No mutex, no allocation, no data race; the GUI always sees
 * the most recent update.
 *
 * Audio thread writes via update(); GUI thread reads via read().
 * Values are in dB, nominally clamped to [-80, 0] by the caller.
 */
class BandLevelBridge
{
public:
    static constexpr int kMaxBands = 256;

    BandLevelBridge() = default;

    /** Audio thread: publish band levels. Wait-free. */
    void update(const float* levelsDb, int numBands) noexcept
    {
#if DTCWPT_ENABLE_TEST_SEAMS
        updateCount_.fetch_add(1, std::memory_order_relaxed);
#endif
        const int n = std::clamp(numBands, 0, kMaxBands);

        Snapshot& snapshot = snapshots_.getWriteBuffer();
        std::copy(levelsDb, levelsDb + n, snapshot.levelsDb.begin());
        snapshot.numBands = n;
        lastNumBands_ = n;
        snapshots_.publish();
    }

    /**
     * GUI thread: read the latest band levels into `outLevelsDb`
     * (room for kMaxBands floats is required). Returns the number of active
     * bands, or 0 before the first update().
     */
    int read(float* outLevelsDb) noexcept
    {
        snapshots_.acquire();

        const Snapshot& snapshot = snapshots_.getReadBuffer();
        std::copy(snapshot.levelsDb.begin(), snapshot.levelsDb.begin() + snapshot.numBands, outLevelsDb);
        return snapshot.numBands;
    }

#if DTCWPT_ENABLE_TEST_SEAMS
    /** Test seam: number of update() calls since construction. */
    std::uint64_t getUpdateCountForTest() const noexcept { return updateCount_.load(std::memory_order_relaxed); }
#endif

    /** Writer side only; preserves band count while publishing silent meters. */
    void reset() noexcept
    {
        std::array<float, kMaxBands> floor;
        floor.fill(-80.0f);
        update(floor.data(), lastNumBands_);
    }

private:
    int lastNumBands_ = 0;
    struct Snapshot
    {
        int numBands = 0;
        std::array<float, kMaxBands> levelsDb {};
    };

    TripleBuffer<Snapshot> snapshots_;
#if DTCWPT_ENABLE_TEST_SEAMS
    std::atomic<std::uint64_t> updateCount_ { 0 };
#endif
};
