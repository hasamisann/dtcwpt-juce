#pragma once

/**
 * @file EngineHandoff.h
 * @brief Lock-free hand-off of fully built engine bundles to the audio thread and back.
 *
 * A "bundle" is whatever the plugin needs to run one topology (engine, band processor
 * observer, analyzer aligner, latency, ...). The bundle is built on a non-audio thread,
 * handed to the audio thread through a single atomic pointer ("pending"), and once the
 * audio thread is done with it, handed back through a fixed-capacity SPSC ring
 * ("retire") so that it is destroyed on a non-audio thread.
 *
 * Thread roles
 *  - Producer (any non-audio thread, but only one at a time: the caller serialises
 *    calls with its own mutex): publish(), reclaim(), discardPending(), outstanding().
 *  - Consumer (the audio thread): acquire(), retire().
 *
 * Guarantees
 *  - The audio thread never locks, allocates or frees: acquire() is one atomic exchange,
 *    retire() writes one slot of a pre-allocated ring.
 *  - Latest wins and nothing is lost: publish() atomically exchanges the pending pointer.
 *    If the audio thread has not taken the previous candidate yet, the producer receives
 *    it back from the exchange and destroys it. If the audio thread took it first, the
 *    exchange returns nullptr and the new candidate is simply picked up next time.
 *    (This is the property the old mutex + dirty-flag TopologyState did not have.)
 *  - The retire ring cannot overflow: every bundle that exists is counted in
 *    outstanding() from publish() until it is destroyed, and publish() refuses to publish
 *    while outstanding() has reached the ring capacity. The ring holds a subset of the
 *    outstanding bundles, so it always has room for retire().
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>

template <typename Bundle, std::size_t RetireCapacity = 16>
class EngineHandoff
{
public:
    static_assert (RetireCapacity >= 4, "Retire ring needs room for the active, fading and pending bundles");

    static constexpr std::size_t kRetireCapacity = RetireCapacity;

    EngineHandoff() = default;

    ~EngineHandoff()
    {
        delete pending_.exchange (nullptr, std::memory_order_acq_rel);
        reclaim();
    }

    EngineHandoff (const EngineHandoff&) = delete;
    EngineHandoff& operator= (const EngineHandoff&) = delete;

    //==========================================================================
    // Producer side (non-audio thread, externally serialised)
    //==========================================================================

    /**
     * @brief Makes `candidate` the pending bundle for the audio thread.
     *
     * Takes ownership only when it returns true. Returns false (and leaves `candidate`
     * untouched) when too many bundles are still alive; the caller keeps the candidate,
     * reclaims, and tries again later.
     */
    bool publish (std::unique_ptr<Bundle>& candidate)
    {
        if (candidate == nullptr || outstanding_ >= RetireCapacity)
            return false;

        Bundle* const replaced = pending_.exchange (candidate.release(), std::memory_order_acq_rel);

        if (replaced != nullptr)
            delete replaced;        // never reached the audio thread: one bundle in, one bundle out
        else
            ++outstanding_;

        return true;
    }

    /** Destroys every bundle the audio thread has retired. Returns how many were destroyed. */
    std::size_t reclaim() noexcept
    {
        std::size_t destroyed = 0;
        std::size_t head = head_.load (std::memory_order_relaxed);
        const std::size_t tail = tail_.load (std::memory_order_acquire);

        while (head != tail)
        {
            Bundle* const bundle = ring_[head % RetireCapacity];
            ++head;
            head_.store (head, std::memory_order_release);
            delete bundle;
            --outstanding_;
            ++destroyed;
        }

        return destroyed;
    }

    /** Destroys the pending bundle if the audio thread has not taken it. Returns true if one was dropped. */
    bool discardPending() noexcept
    {
        Bundle* const dropped = pending_.exchange (nullptr, std::memory_order_acq_rel);
        if (dropped == nullptr)
            return false;

        delete dropped;
        --outstanding_;
        return true;
    }

    /** Number of bundles that were published and not destroyed yet (pending + in use + retired). */
    std::size_t outstanding() const noexcept { return outstanding_; }

    //==========================================================================
    // Consumer side (audio thread)
    //==========================================================================

    /** Takes the pending bundle, or nullptr. The caller owns it and must hand it back through retire(). */
    Bundle* acquire() noexcept
    {
        if (pending_.load (std::memory_order_relaxed) == nullptr)
            return nullptr;

        return pending_.exchange (nullptr, std::memory_order_acq_rel);
    }

    /**
     * @brief Hands a bundle that is no longer used back for destruction on a non-audio thread.
     *
     * Cannot fail for bundles that came from publish() (see the class comment). Returns false
     * only if that invariant is broken; the bundle is then not enqueued.
     */
    bool retire (Bundle* bundle) noexcept
    {
        const std::size_t tail = tail_.load (std::memory_order_relaxed);
        const std::size_t head = head_.load (std::memory_order_acquire);

        if (tail - head >= RetireCapacity)
        {
            retireOverflows_.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        ring_[tail % RetireCapacity] = bundle;
        tail_.store (tail + 1, std::memory_order_release);
        return true;
    }

    /** Number of retire() calls that found the ring full. Always 0 unless the invariant is broken. */
    std::size_t retireOverflowCount() const noexcept { return retireOverflows_.load (std::memory_order_relaxed); }

private:
    std::atomic<Bundle*> pending_ { nullptr };

    std::array<Bundle*, RetireCapacity> ring_ {};
    std::atomic<std::size_t> head_ { 0 };   // advanced by reclaim() (producer thread)
    std::atomic<std::size_t> tail_ { 0 };   // advanced by retire() (audio thread)
    std::atomic<std::size_t> retireOverflows_ { 0 };

    std::size_t outstanding_ { 0 };         // producer thread only
};
