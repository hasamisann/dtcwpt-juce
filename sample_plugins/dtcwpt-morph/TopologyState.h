#pragma once

/**
 * @file TopologyState.h
 * @brief Thread-safe topology handoff between GUI and audio threads.
 *
 * `TopologyState` implements a single-producer, single-consumer handoff pattern:
 *
 * - **GUI thread** calls `requestChange()` to store new destination strings and
 *   set the `dirty` flag.
 * - **Audio thread** calls `tryConsume()` each `processBlock()`. If the flag is
 *   set, the destinations are copied out, the flag is cleared, and `true` is
 *   returned. Otherwise `false` is returned immediately (no blocking).
 *
 * The `mutex` protects only the `destinations` vector — the `dirty` flag itself
 * is an `std::atomic<bool>` and never requires the mutex.
 *
 * Thread ownership:
 * - `requestChange()` — GUI thread only.
 * - `tryConsume()`    — Audio thread only.
 *
 * This struct is non-copyable and non-movable because it owns a `std::mutex`
 * and an `std::atomic<bool>`.
 */

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

struct TopologyState
{
    //==============================================================================
    // Public state (atomic — visible without locking)
    //==============================================================================

    /** Set by the GUI thread after a topology change; cleared by the audio thread. */
    std::atomic<bool> dirty { false };

    //==============================================================================
    // API
    //==============================================================================

    /**
     * @brief Store new destinations and mark the state as dirty.
     *
     * Called on the **GUI thread** after the user edits the topology tree.
     * Locks `mutex`, copies `newDestinations` into the internal vector, then
     * atomically sets `dirty = true`.
     *
     * @param newDestinations New set of destination path strings (e.g., {"H","LH","LL"}).
     */
    void requestChange (const std::vector<std::string>& newDestinations) noexcept
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            destinations = newDestinations;
        }
        dirty.store (true, std::memory_order_release);
    }

    /**
     * @brief Consume pending topology change if one is available.
     *
     * Called on the **audio thread** at the start of each `processBlock()`.
     * If `dirty` is true, locks `mutex`, copies destinations to `out`, clears
     * `dirty`, and returns `true`. If `dirty` is false, returns immediately
     * without locking.
     *
     * @param out  Receives the new destinations if the flag was set.
     * @return     `true` if a new topology was consumed; `false` otherwise.
     */
    bool tryConsume (std::vector<std::string>& out) noexcept
    {
        if (! dirty.load (std::memory_order_acquire))
            return false;

        {
            std::lock_guard<std::mutex> lock (mutex);
            out = destinations;
        }
        dirty.store (false, std::memory_order_release);
        return true;
    }

    //==============================================================================
    // Non-copyable / non-movable
    //==============================================================================

    TopologyState()                                  = default;
    ~TopologyState()                                 = default;
    TopologyState (const TopologyState&)             = delete;
    TopologyState& operator= (const TopologyState&) = delete;
    TopologyState (TopologyState&&)                  = delete;
    TopologyState& operator= (TopologyState&&)       = delete;

private:
    std::mutex               mutex;
    std::vector<std::string> destinations; ///< Protected by mutex
};
