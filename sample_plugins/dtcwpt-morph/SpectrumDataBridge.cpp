/**
 * @file SpectrumDataBridge.cpp
 * @brief Lock-free triple-buffer implementation for audio→GUI spectrum data.
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

#if DTCWPT_ENABLE_TEST_SEAMS
    pushCallCount_.fetch_add (1, std::memory_order_relaxed);
#endif

    int remaining = numSamples;
    int srcOffset = 0;

    while (remaining > 0)
    {
        Slot& slot = slots_.getWriteBuffer();

        const int toCopy = std::min (remaining, kSlotSize - fill_);

        std::memcpy (slot.input.data()  + fill_, inputMono  + srcOffset,
                     static_cast<std::size_t> (toCopy) * sizeof (float));
        std::memcpy (slot.output.data() + fill_, outputMono + srcOffset,
                     static_cast<std::size_t> (toCopy) * sizeof (float));

        fill_     += toCopy;
        srcOffset += toCopy;
        remaining -= toCopy;

        if (fill_ >= kSlotSize)
        {
            // Hand the completed slot over and continue in the buffer we get back.
            slots_.publish();
            fill_ = 0;
        }
    }
}

//==============================================================================
// GUI-thread API
//==============================================================================

bool SpectrumDataBridge::tryConsume (std::array<float, kSlotSize>& inputOut,
                                      std::array<float, kSlotSize>& outputOut)
{
    if (! slots_.acquire())
        return false;  // No new completed slot

    // The slot returned by getReadBuffer() belongs to this thread until the
    // next successful acquire(), so the writer cannot touch it while we copy.
    const Slot& slot = slots_.getReadBuffer();

    inputOut = slot.input;
#if DTCWPT_ENABLE_TEST_SEAMS
    if (consumeMidCopyHook_)
        consumeMidCopyHook_();
#endif
    outputOut = slot.output;
    return true;
}
