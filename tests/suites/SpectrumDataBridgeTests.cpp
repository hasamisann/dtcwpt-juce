/**
 * @file SpectrumDataBridgeTests.cpp
 * @brief JUCE UnitTest suite for SpectrumDataBridge.
 *
 * Single-threaded deterministic tests covering:
 *  1. No data initially: tryConsume returns false.
 *  2. Push exactly one full slot: tryConsume returns true with correct data.
 *  3. Partial push does not complete slot; completing it returns true.
 *  4. Multiple pushes (chunks of 256) accumulate correctly.
 *  5. Overwrite: 4 full slots pushed without consuming — tryConsume returns latest.
 *  6. tryConsume clears flag: second call returns false until new data pushed.
 *  7. A snapshot being read is not overwritten when the writer laps the ring
 *     (deterministic, via the mid-copy test seam).
 *  8. A slot completed while the reader is mid-consume is not lost
 *     (deterministic, via the mid-copy test seam).
 *  9. Concurrent writer/reader stress: no torn snapshots, tags never go
 *     backwards, and the final slot is always observable (run under TSan).
 */

#include "SpectrumDataBridge.h"
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

class SpectrumDataBridgeTests : public juce::UnitTest
{
public:
    SpectrumDataBridgeTests() : UnitTest ("SpectrumDataBridgeTests") {}

    void runTest() override
    {
        beginTest ("1. No data initially: tryConsume returns false");
        testNoDataInitially();

        beginTest ("2. Push exactly one full slot: tryConsume returns true with data");
        testPushOneSlot();

        beginTest ("3. Partial push does not complete slot; completing it returns true");
        testPartialPush();

        beginTest ("4. Multiple chunk pushes accumulate correctly (8 × 256 = 2048)");
        testChunkedPush();

        beginTest ("5. Overwrite: push 4 slots without consuming — latest data returned");
        testOverwrite();

        beginTest ("6. tryConsume clears flag; second call returns false");
        testConsumeClearsFlag();

        beginTest ("7. Reader mid-copy is not overwritten when the writer laps the ring");
        testReaderIsNotOverwrittenWhileConsuming();

        beginTest ("8. Slot completed during tryConsume is not lost");
        testSlotCompletedDuringConsumeIsNotLost();

        beginTest ("9. Concurrent writer/reader stress keeps snapshots consistent");
        testConcurrentStress();
    }

private:
    // -----------------------------------------------------------------------
    // Utility: generate a deterministic float ramp pattern for a given slot tag.
    // inputVal[i]  = tag + i / 100000.f
    // outputVal[i] = tag - i / 100000.f
    static void makeRamp (float tag,
                           std::vector<float>& inp,
                           std::vector<float>& out,
                           int numSamples)
    {
        inp.resize (static_cast<std::size_t> (numSamples));
        out.resize (static_cast<std::size_t> (numSamples));
        for (int i = 0; i < numSamples; ++i)
        {
            inp[static_cast<std::size_t> (i)] = tag + static_cast<float> (i) * 0.00001f;
            out[static_cast<std::size_t> (i)] = tag - static_cast<float> (i) * 0.00001f;
        }
    }

    // -----------------------------------------------------------------------
    void testNoDataInitially()
    {
        SpectrumDataBridge bridge;
        std::array<float, SpectrumDataBridge::kSlotSize> inp {}, outp {};
        expect (! bridge.tryConsume (inp, outp), "tryConsume should return false when no data");
    }

    // -----------------------------------------------------------------------
    void testPushOneSlot()
    {
        SpectrumDataBridge bridge;

        std::vector<float> inp, outp;
        makeRamp (1.0f, inp, outp, SpectrumDataBridge::kSlotSize);

        bridge.push (inp.data(), outp.data(), SpectrumDataBridge::kSlotSize);

        std::array<float, SpectrumDataBridge::kSlotSize> inOut {}, outOut {};
        expect (bridge.tryConsume (inOut, outOut), "tryConsume should return true after full slot");

        constexpr float tol = 1e-7f;
        for (int i = 0; i < SpectrumDataBridge::kSlotSize; ++i)
        {
            auto is = static_cast<std::size_t> (i);
            expectWithinAbsoluteError (inOut[is],  inp[is],  tol, "Input sample mismatch");
            expectWithinAbsoluteError (outOut[is], outp[is], tol, "Output sample mismatch");
        }
    }

    // -----------------------------------------------------------------------
    void testPartialPush()
    {
        SpectrumDataBridge bridge;

        std::vector<float> inp, outp;
        makeRamp (2.0f, inp, outp, SpectrumDataBridge::kSlotSize);

        // Push first half — slot not complete yet
        const int half = SpectrumDataBridge::kSlotSize / 2;
        bridge.push (inp.data(), outp.data(), half);

        std::array<float, SpectrumDataBridge::kSlotSize> inOut {}, outOut {};
        expect (! bridge.tryConsume (inOut, outOut),
                "tryConsume should return false after partial push");

        // Push second half — slot now complete
        bridge.push (inp.data() + half, outp.data() + half, half);

        expect (bridge.tryConsume (inOut, outOut),
                "tryConsume should return true after slot is completed");

        constexpr float tol = 1e-7f;
        for (int i = 0; i < SpectrumDataBridge::kSlotSize; ++i)
        {
            auto is = static_cast<std::size_t> (i);
            expectWithinAbsoluteError (inOut[is],  inp[is],  tol, "Reassembled input mismatch");
            expectWithinAbsoluteError (outOut[is], outp[is], tol, "Reassembled output mismatch");
        }
    }

    // -----------------------------------------------------------------------
    void testChunkedPush()
    {
        SpectrumDataBridge bridge;

        std::vector<float> inp, outp;
        makeRamp (3.0f, inp, outp, SpectrumDataBridge::kSlotSize);

        // Push 8 chunks of 256 samples each
        constexpr int kChunk = 256;
        for (int i = 0; i < SpectrumDataBridge::kSlotSize / kChunk; ++i)
            bridge.push (inp.data() + i * kChunk, outp.data() + i * kChunk, kChunk);

        std::array<float, SpectrumDataBridge::kSlotSize> inOut {}, outOut {};
        expect (bridge.tryConsume (inOut, outOut), "tryConsume should return true after 8 chunks");

        constexpr float tol = 1e-7f;
        for (int i = 0; i < SpectrumDataBridge::kSlotSize; ++i)
        {
            auto is = static_cast<std::size_t> (i);
            expectWithinAbsoluteError (inOut[is],  inp[is],  tol, "Chunked input mismatch");
            expectWithinAbsoluteError (outOut[is], outp[is], tol, "Chunked output mismatch");
        }
    }

    // -----------------------------------------------------------------------
    void testOverwrite()
    {
        SpectrumDataBridge bridge;

        // Push 4 full slots — the 4th is the "latest"
        for (int slot = 0; slot < 4; ++slot)
        {
            std::vector<float> inp, outp;
            makeRamp (static_cast<float> (slot + 1) * 10.0f, inp, outp,
                      SpectrumDataBridge::kSlotSize);
            bridge.push (inp.data(), outp.data(), SpectrumDataBridge::kSlotSize);
        }

        // Build expected data for slot 4 (tag = 40.0f)
        std::vector<float> expInp, expOutp;
        makeRamp (40.0f, expInp, expOutp, SpectrumDataBridge::kSlotSize);

        std::array<float, SpectrumDataBridge::kSlotSize> inOut {}, outOut {};
        expect (bridge.tryConsume (inOut, outOut), "tryConsume should return true after overwrite");

        constexpr float tol = 1e-7f;
        // Only spot-check a few samples — verifies we got the most-recent slot
        expectWithinAbsoluteError (inOut[0],   expInp[0],   tol, "Overwrite: first input sample");
        expectWithinAbsoluteError (inOut[100],  expInp[100],  tol, "Overwrite: mid input sample");
        expectWithinAbsoluteError (outOut[0],  expOutp[0],  tol, "Overwrite: first output sample");
    }

    // -----------------------------------------------------------------------
    void testConsumeClearsFlag()
    {
        SpectrumDataBridge bridge;

        std::vector<float> inp, outp;
        makeRamp (5.0f, inp, outp, SpectrumDataBridge::kSlotSize);
        bridge.push (inp.data(), outp.data(), SpectrumDataBridge::kSlotSize);

        std::array<float, SpectrumDataBridge::kSlotSize> inOut {}, outOut {};
        expect (bridge.tryConsume (inOut, outOut), "First consume should succeed");
        expect (! bridge.tryConsume (inOut, outOut),
                "Second tryConsume should return false (already consumed)");

        // Push another slot — should be consumable again
        bridge.push (inp.data(), outp.data(), SpectrumDataBridge::kSlotSize);
        expect (bridge.tryConsume (inOut, outOut), "tryConsume should succeed after new data");
    }

    // -----------------------------------------------------------------------
    // Pushes one full slot whose samples are all `tag` (input) / `-tag` (output).
    static void pushConstantSlot (SpectrumDataBridge& bridge, float tag)
    {
        std::vector<float> inp (static_cast<std::size_t> (SpectrumDataBridge::kSlotSize), tag);
        std::vector<float> outp (static_cast<std::size_t> (SpectrumDataBridge::kSlotSize), -tag);
        bridge.push (inp.data(), outp.data(), SpectrumDataBridge::kSlotSize);
    }

    // -----------------------------------------------------------------------
    void testReaderIsNotOverwrittenWhileConsuming()
    {
#if DTCWPT_ENABLE_TEST_SEAMS
        SpectrumDataBridge bridge;
        pushConstantSlot (bridge, 1.0f);

        // Four slots is enough to lap the original 4-slot ring.
        constexpr int kLapSlots = 4;

        // While the reader is between the input copy and the output copy, the
        // audio thread completes four more slots, which wraps the whole ring.
        bridge.setConsumeMidCopyHookForTest ([&]
        {
            for (int i = 0; i < kLapSlots; ++i)
                pushConstantSlot (bridge, 2.0f + static_cast<float> (i));
        });

        std::array<float, SpectrumDataBridge::kSlotSize> inOut {}, outOut {};
        expect (bridge.tryConsume (inOut, outOut), "tryConsume should return true");
        bridge.setConsumeMidCopyHookForTest (nullptr);

        expectEquals (inOut.front(), 1.0f, "Input half should come from the first slot");
        expectEquals (outOut.front(), -1.0f,
                      "Output half must come from the same slot as the input half "
                      "(the slot being read must not be overwritten)");
        expectEquals (outOut.back(), -1.0f, "Output half must not be partially overwritten");

        // The newest completed slot (tag 5) must still be delivered afterwards.
        expect (bridge.tryConsume (inOut, outOut), "The latest slot should be consumable afterwards");
        expectEquals (inOut.front(), 5.0f, "Latest slot input");
        expectEquals (outOut.front(), -5.0f, "Latest slot output");
#else
        expect (false, "DTCWPT_ENABLE_TEST_SEAMS must be enabled");
#endif
    }

    // -----------------------------------------------------------------------
    void testSlotCompletedDuringConsumeIsNotLost()
    {
#if DTCWPT_ENABLE_TEST_SEAMS
        SpectrumDataBridge bridge;
        pushConstantSlot (bridge, 1.0f);

        bridge.setConsumeMidCopyHookForTest ([&] { pushConstantSlot (bridge, 2.0f); });

        std::array<float, SpectrumDataBridge::kSlotSize> inOut {}, outOut {};
        expect (bridge.tryConsume (inOut, outOut), "First tryConsume should return true");
        bridge.setConsumeMidCopyHookForTest (nullptr);
        expectEquals (inOut.front(), 1.0f, "First consume returns the first slot");

        expect (bridge.tryConsume (inOut, outOut),
                "A slot completed during the previous tryConsume must still be reported as new");
        expectEquals (inOut.front(), 2.0f, "Second consume returns the slot completed mid-consume");
#else
        expect (false, "DTCWPT_ENABLE_TEST_SEAMS must be enabled");
#endif
    }

    // -----------------------------------------------------------------------
    void testConcurrentStress()
    {
        SpectrumDataBridge bridge;
        constexpr int kSlotsToWrite = 4000;

        std::atomic<bool> writerDone { false };

        std::thread writer ([&]
        {
            // Odd chunk sizes so that slot boundaries fall inside push() calls.
            const int chunkSizes[] = { 64, 137, 512, 1000, 2048, 3001 };
            std::vector<float> inp (4096), outp (4096);
            int chunkIndex = 0;
            int sampleInSlot = 0;
            int tag = 1;

            while (tag <= kSlotsToWrite)
            {
                const int want = chunkSizes[chunkIndex++ % 6];
                int n = 0;
                // Fill `n` samples (never crossing more than the remaining tag range).
                while (n < want && tag <= kSlotsToWrite)
                {
                    inp[static_cast<std::size_t> (n)]  = static_cast<float> (tag);
                    outp[static_cast<std::size_t> (n)] = -static_cast<float> (tag);
                    ++n;
                    if (++sampleInSlot == SpectrumDataBridge::kSlotSize)
                    {
                        sampleInSlot = 0;
                        ++tag;
                    }
                }
                bridge.push (inp.data(), outp.data(), n);
            }
            writerDone.store (true, std::memory_order_release);
        });

        int tornSnapshots = 0;
        int backwards = 0;
        int received = 0;
        float lastTag = 0.0f;
        std::array<float, SpectrumDataBridge::kSlotSize> inOut {}, outOut {};

        auto checkSnapshot = [&]
        {
            ++received;
            const float tag = inOut.front();
            bool torn = false;
            for (std::size_t i = 0; i < inOut.size(); ++i)
                if (inOut[i] != tag || outOut[i] != -tag)
                    torn = true;
            if (torn)
                ++tornSnapshots;
            if (tag < lastTag)
                ++backwards;
            lastTag = tag;
        };

        while (! writerDone.load (std::memory_order_acquire))
            if (bridge.tryConsume (inOut, outOut))
                checkSnapshot();

        writer.join();

        // Drain: the very last completed slot must be observable.
        if (bridge.tryConsume (inOut, outOut))
            checkSnapshot();

        expectEquals (tornSnapshots, 0, "No snapshot may mix samples from different slots");
        expectEquals (backwards, 0, "Snapshot tags must never go backwards");
        expect (received > 0, "Reader should receive at least one snapshot");
        expectEquals (lastTag, static_cast<float> (kSlotsToWrite),
                      "The final completed slot must not be lost");
    }
};

// Static registration
static SpectrumDataBridgeTests spectrumDataBridgeTests;
