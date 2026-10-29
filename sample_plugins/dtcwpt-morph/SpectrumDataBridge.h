#pragma once

/**
 * @file SpectrumDataBridge.h
 * @brief Lock-free audio→GUI transfer of spectrum-analyzer sample windows.
 *
 * `SpectrumDataBridge` transfers mono-averaged float samples from the
 * **audio thread** to the **GUI thread** without blocking either thread.
 *
 * ## Design
 *
 * - Built on `TripleBuffer`: the audio thread owns a private back buffer, the
 *   GUI thread owns a private front buffer, and a third buffer is exchanged
 *   atomically between them. No buffer is ever read and written concurrently.
 * - A window ("slot") holds `kSlotSize` input and `kSlotSize` output samples
 *   (matching the FFT window size used by `SpectrumAnalyzerWidget`).
 * - The audio thread accumulates samples via `push()` directly into its back
 *   buffer. When `kSlotSize` samples have been collected the buffer is
 *   published and a fresh back buffer is started; a single `push()` may span
 *   several slot boundaries.
 * - The GUI thread takes the latest completed slot via `tryConsume()`. Slots
 *   that were completed but not consumed are superseded by newer ones (the
 *   GUI only ever needs the latest window). A slot completed while the GUI is
 *   in the middle of `tryConsume()` is never lost: it is reported by the next
 *   call.
 * - No mutex, no allocation, no data race.
 *
 * ## Thread ownership
 * - `push()`, `discardPartialSlot()` — **audio thread only**
 * - `tryConsume()`                   — **GUI thread only**
 */

#include "../common/TripleBuffer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>

class SpectrumDataBridge
{
public:
    //==============================================================================
    // Constants
    //==============================================================================

    static constexpr int kSlotSize  = 2048;   ///< Samples per slot (matches FFT size)

    //==============================================================================
    // Constructor
    //==============================================================================

    SpectrumDataBridge() = default;

    //==============================================================================
    // Audio-thread API
    //==============================================================================

    /**
     * @brief Accumulate mono-averaged samples into the slot being filled.
     *
     * Called on the **audio thread**. Copies `numSamples` samples from
     * `inputMono` and `outputMono` into the audio thread's private buffer. Each
     * time the buffer reaches `kSlotSize` samples it is published to the GUI
     * thread and a new slot is started, so a single call can span a slot
     * boundary (e.g., a 512-sample block can finish one slot and start the
     * next).
     *
     * @param inputMono   Mono-averaged input samples  (may be nullptr if numSamples == 0)
     * @param outputMono  Mono-averaged output samples (may be nullptr if numSamples == 0)
     * @param numSamples  Number of samples to push
     */
    void push (const float* inputMono, const float* outputMono, int numSamples);

    /**
     * @brief Drops the samples collected for the slot that is not yet complete.
     *
     * Called on the **audio thread** when the sample stream is discontinuous
     * (e.g. after display data generation was paused), so that a slot never
     * mixes samples from before and after the gap. Already published slots are
     * unaffected.
     */
    void reset() noexcept
    {
        auto& slot = slots_.getWriteBuffer();
        slot.input.fill(0.0f); slot.output.fill(0.0f);
        slots_.publish();
        fill_ = 0;
    }

    void discardPartialSlot() noexcept { fill_ = 0; }

    //==============================================================================
    // GUI-thread API
    //==============================================================================

    /**
     * @brief Read the latest completed slot.
     *
     * Called on the **GUI thread**. If at least one slot has been completed
     * since the last successful call, copies the most recently completed slot
     * into `inputOut` and `outputOut` and returns `true`. Returns `false`
     * immediately if there is no new data.
     *
     * @param inputOut   Receives the input samples from the latest completed slot.
     * @param outputOut  Receives the output samples from the latest completed slot.
     * @return           `true` if new data was available and copied; `false` otherwise.
     */
    bool tryConsume (std::array<float, kSlotSize>& inputOut,
                     std::array<float, kSlotSize>& outputOut);

#if DTCWPT_ENABLE_TEST_SEAMS
    /** Test seam: called by tryConsume() after the input half has been copied and before the output half. */
    void setConsumeMidCopyHookForTest (std::function<void()> hook) { consumeMidCopyHook_ = std::move (hook); }

    /** Test seam: number of push() calls with numSamples > 0 since construction. */
    std::uint64_t getPushCallCountForTest() const noexcept { return pushCallCount_.load (std::memory_order_relaxed); }
#endif

private:
    struct Slot
    {
        std::array<float, kSlotSize> input  {};   ///< Input mono samples
        std::array<float, kSlotSize> output {};   ///< Output mono samples
    };

    TripleBuffer<Slot> slots_;

    /// Samples already written into slots_.getWriteBuffer() (audio thread only).
    int fill_ { 0 };

#if DTCWPT_ENABLE_TEST_SEAMS
    std::function<void()> consumeMidCopyHook_;
    std::atomic<std::uint64_t> pushCallCount_ { 0 };
#endif
};
