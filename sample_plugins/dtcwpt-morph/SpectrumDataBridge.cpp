/**
 * @file SpectrumDataBridge.cpp
 * @brief Lock-free ring buffer implementation for audio→GUI spectrum data.
 */

#include "SpectrumDataBridge.h"

#include <algorithm>
#include <cstring>

//==============================================================================
// Audio-thread API
//==============================================================================

void SpectrumDataBridge::push (const float* inputMono,
                                const float* outputMono,
                                int          numSamples)
{
    if (numSamples <= 0)
        return;

    int remaining = numSamples;
    int srcOffset = 0;

    while (remaining > 0)
    {
        const int writeIdx = writeSlot_.load (std::memory_order_relaxed);
        Slot&     slot     = slots_[static_cast<std::size_t> (writeIdx)];

        // How many samples can we write into the current slot?
        const int available = kSlotSize - slot.fill.load (std::memory_order_relaxed);
        const int toCopy    = std::min (remaining, available);

        // Copy samples into the current write slot
        const int currentFill = slot.fill.load (std::memory_order_relaxed);
        std::memcpy (slot.input.data()  + currentFill, inputMono  + srcOffset,
                     static_cast<std::size_t> (toCopy) * sizeof (float));
        std::memcpy (slot.output.data() + currentFill, outputMono + srcOffset,
                     static_cast<std::size_t> (toCopy) * sizeof (float));

        slot.fill.store (currentFill + toCopy, std::memory_order_relaxed);
        srcOffset += toCopy;
        remaining -= toCopy;

        // If the slot is now full, advance writeSlot_ to the next slot
        if (slot.fill.load (std::memory_order_relaxed) >= kSlotSize)
        {
            const int nextSlot = (writeIdx + 1) % kRingSlots;
            // Reset the next slot's fill counter before advancing writeSlot_
            // so the GUI thread never sees a partially-reset next slot.
            // The subsequent writeSlot_.store(release) provides the necessary
            // ordering to make this reset visible to the GUI thread.
            slots_[static_cast<std::size_t> (nextSlot)].fill.store (0, std::memory_order_relaxed);
            writeSlot_.store (nextSlot, std::memory_order_release);
            // Signal that new completed data is available
            hasNewData_.store (true, std::memory_order_release);
        }
    }
}

//==============================================================================
// GUI-thread API
//==============================================================================

bool SpectrumDataBridge::tryConsume (std::array<float, kSlotSize>& inputOut,
                                      std::array<float, kSlotSize>& outputOut)
{
    // Use hasNewData_ flag to detect new data, which correctly handles the
    // case where writeSlot_ has wrapped around to equal readSlot_.
    if (! hasNewData_.load (std::memory_order_acquire))
        return false;  // No new completed slot

    const int write = writeSlot_.load (std::memory_order_acquire);

    // The most-recently completed slot is the one just before writeSlot_.
    const int latestCompleted = (write - 1 + kRingSlots) % kRingSlots;
    const Slot& slot = slots_[static_cast<std::size_t> (latestCompleted)];

    inputOut  = slot.input;
    outputOut = slot.output;

    // Clear the new-data flag and advance readSlot_ to match writeSlot_
    hasNewData_.store (false, std::memory_order_release);
    readSlot_.store (write, std::memory_order_release);
    return true;
}
