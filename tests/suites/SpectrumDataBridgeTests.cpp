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
 */

#include "SpectrumDataBridge.h"
#include <juce_core/juce_core.h>
#include <array>
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
};

// Static registration
static SpectrumDataBridgeTests spectrumDataBridgeTests;
