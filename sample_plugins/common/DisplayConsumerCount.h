#pragma once

/**
 * @file DisplayConsumerCount.h
 * @brief Tells the audio thread whether anything is displaying its analysis data.
 *
 * The audio thread only builds display-only data (mono mix-down, analyzer
 * alignment, spectrum / band-level bridge writes) while at least one consumer,
 * normally an open editor, is registered.
 *
 * ## Thread ownership
 * - `add()` / `remove()` / `Scoped` — GUI (message) thread
 * - `isActive()`                    — any thread, wait-free (audio thread)
 */

#include <atomic>

class DisplayConsumerCount
{
public:
    void add() noexcept    { count_.fetch_add (1, std::memory_order_acq_rel); }
    void remove() noexcept { count_.fetch_sub (1, std::memory_order_acq_rel); }

    bool isActive() const noexcept { return count_.load (std::memory_order_acquire) > 0; }

    /** Registers a consumer for the lifetime of this object. */
    class Scoped
    {
    public:
        explicit Scoped (DisplayConsumerCount& count) noexcept : count_ (count) { count_.add(); }
        ~Scoped() { count_.remove(); }

        Scoped (const Scoped&) = delete;
        Scoped& operator= (const Scoped&) = delete;

    private:
        DisplayConsumerCount& count_;
    };

private:
    std::atomic<int> count_ { 0 };
};
