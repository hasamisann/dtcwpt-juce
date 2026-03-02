#pragma once

/**
 * @file SpectrumDataBridge.h
 * @brief Lock-free ring buffer for audio→GUI spectrum data transfer.
 *
 * `SpectrumDataBridge` transfers mono-averaged float samples from the
 * **audio thread** to the **GUI thread** without blocking either thread.
 *
 * ## Design
 *
 * - **4 fixed-size slots**, each holding 2048 input and 2048 output samples
 *   (matching the FFT window size used by `SpectrumAnalyzerWidget`).
 * - The audio thread accumulates samples via `push()`. When a slot fills to
 *   `kSlotSize` samples, `writeSlot_` advances to the next slot (modulo 4)
 *   and the new slot's fill counter is reset to zero.
 * - The GUI thread reads the latest completed slot via `tryConsume()`. If
 *   `readSlot_` differs from `writeSlot_`, the slot just before `writeSlot_`
 *   is the most recently completed one — it is copied out and `readSlot_` is
 *   advanced to `writeSlot_`.
 * - Overwriting unread slots is acceptable: the GUI only ever needs the
 *   latest spectrum snapshot (no history required).
 * - The audio side uses only `std::atomic` indices (no mutex) — true
 *   lock-free behaviour on the audio thread.
 *
 * ## Thread ownership
 * - `push()`       — **audio thread only**
 * - `tryConsume()` — **GUI thread only**
 */

#include <array>
#include <atomic>

class SpectrumDataBridge
{
public:
    //==============================================================================
    // Constants
    //==============================================================================

    static constexpr int kRingSlots = 4;      ///< Number of ring slots (power of 2)
    static constexpr int kSlotSize  = 2048;   ///< Samples per slot (matches FFT size)

    //==============================================================================
    // Constructor
    //==============================================================================

    SpectrumDataBridge() = default;

    //==============================================================================
    // Audio-thread API
    //==============================================================================

    /**
     * @brief Accumulate mono-averaged samples into the current write slot.
     *
     * Called on the **audio thread**. Copies up to `numSamples` samples from
     * `inputMono` and `outputMono` into the current write slot. When the slot
     * fills to `kSlotSize` samples, `writeSlot_` advances and the new slot's
     * fill counter is reset.
     *
     * Processes samples in chunks so that a single call can span a slot
     * boundary (e.g., a 512-sample block can finish one slot and start the
     * next in a single `push()`).
     *
     * @param inputMono   Mono-averaged input samples  (may be nullptr if numSamples == 0)
     * @param outputMono  Mono-averaged output samples (may be nullptr if numSamples == 0)
     * @param numSamples  Number of samples to push
     */
    void push (const float* inputMono, const float* outputMono, int numSamples);

    //==============================================================================
    // GUI-thread API
    //==============================================================================

    /**
     * @brief Read the latest completed slot.
     *
     * Called on the **GUI thread**. If `readSlot_` != `writeSlot_` (i.e., at
     * least one new slot has been completed since the last consume), copies
     * the most-recently completed slot's data into `inputOut` and `outputOut`,
     * advances `readSlot_` to `writeSlot_`, and returns `true`.
     *
     * Returns `false` immediately if no new data is available.
     *
     * @param inputOut   Receives the input samples from the latest completed slot.
     * @param outputOut  Receives the output samples from the latest completed slot.
     * @return           `true` if new data was available and copied; `false` otherwise.
     */
    bool tryConsume (std::array<float, kSlotSize>& inputOut,
                     std::array<float, kSlotSize>& outputOut);

private:
    //==============================================================================
    // Internal slot type
    //==============================================================================

    struct Slot
    {
        std::array<float, kSlotSize> input  {};   ///< Input mono samples
        std::array<float, kSlotSize> output {};   ///< Output mono samples
        int fill { 0 };                           ///< Number of samples filled (0..kSlotSize)
    };

    //==============================================================================
    // State
    //==============================================================================

    std::array<Slot, kRingSlots> slots_;

    /// Index of the slot currently being written (audio thread).
    /// A slot is "completed" when push() advances writeSlot_ past it.
    std::atomic<int> writeSlot_ { 0 };

    /// Index of the last slot read by the GUI thread.
    /// tryConsume() advances this to match writeSlot_ when new data exists.
    std::atomic<int> readSlot_  { 0 };

    /// Set to true by push() whenever a slot completes; cleared by tryConsume().
    /// Needed to distinguish "no new data" from the case where writeSlot_
    /// has wrapped all the way around back to equal readSlot_.
    std::atomic<bool> hasNewData_ { false };
};
