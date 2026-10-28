#pragma once

/**
 * @file TripleBuffer.h
 * @brief Wait-free single-producer / single-consumer triple buffer.
 *
 * Hands the *latest* complete value of `T` from one writer thread (the audio
 * thread) to one reader thread (the GUI thread) without locks, allocation or
 * data races on the payload.
 *
 * ## Design
 *
 * Three buffers exist at any time, and each is owned by exactly one party:
 *
 * - the **back** buffer, owned by the writer (it fills it freely);
 * - the **front** buffer, owned by the reader (it reads it freely);
 * - the **middle** buffer, in transit; its index and a "fresh" bit live in a
 *   single `std::atomic` so that exchanging them is one atomic operation.
 *
 * `publish()` swaps back <-> middle and sets the fresh bit.
 * `acquire()` swaps front <-> middle only if the fresh bit is set (and the
 * swap clears it). Neither side ever touches a buffer the other side owns, so
 * the payload needs no synchronisation of its own, a reader that holds a
 * buffer for a long time is never overwritten, and an update published while
 * the reader is busy is never lost (it stays in the middle until the next
 * `acquire()`).
 *
 * ## Thread ownership
 * - `getWriteBuffer()`, `publish()` — **writer thread only**
 * - `acquire()`, `getReadBuffer()`  — **reader thread only**
 */

#include <atomic>
#include <cstdint>

template <typename T>
class TripleBuffer
{
public:
    TripleBuffer() = default;

    TripleBuffer (const TripleBuffer&) = delete;
    TripleBuffer& operator= (const TripleBuffer&) = delete;

    //==============================================================================
    // Writer-thread API
    //==============================================================================

    /** Buffer the writer may fill. Its previous contents are stale (an older value). */
    T& getWriteBuffer() noexcept { return buffers_[writeIndex_]; }

    /** Makes the contents of getWriteBuffer() the newest value visible to the reader. */
    void publish() noexcept
    {
        const std::uint8_t previousMiddle =
            middle_.exchange (static_cast<std::uint8_t> (writeIndex_ | kFreshBit), std::memory_order_acq_rel);
        writeIndex_ = previousMiddle & kIndexMask;
    }

    //==============================================================================
    // Reader-thread API
    //==============================================================================

    /**
     * Takes the newest published value, if any, as the new read buffer.
     * @return true if a value published since the previous successful call was taken;
     *         false (and getReadBuffer() is unchanged) otherwise.
     */
    bool acquire() noexcept
    {
        if ((middle_.load (std::memory_order_acquire) & kFreshBit) == 0)
            return false;

        // Only the reader clears the fresh bit, so the exchange below always
        // returns a fresh index (possibly of an even newer value than the one
        // that was just observed).
        const std::uint8_t previousMiddle =
            middle_.exchange (static_cast<std::uint8_t> (readIndex_), std::memory_order_acq_rel);
        readIndex_ = previousMiddle & kIndexMask;
        return true;
    }

    /** Buffer holding the value taken by the most recent successful acquire() (default-constructed before). */
    const T& getReadBuffer() const noexcept { return buffers_[readIndex_]; }

private:
    static constexpr std::uint8_t kFreshBit  = 0x80;
    static constexpr std::uint8_t kIndexMask = 0x03;

    static_assert (std::atomic<std::uint8_t>::is_always_lock_free,
                   "TripleBuffer requires a lock-free atomic index");

    T buffers_[3] {};

    alignas (64) std::atomic<std::uint8_t> middle_ { 1 };   ///< middle buffer index | fresh bit
    alignas (64) int writeIndex_ { 0 };                      ///< back buffer (writer thread only)
    alignas (64) int readIndex_  { 2 };                      ///< front buffer (reader thread only)
};
