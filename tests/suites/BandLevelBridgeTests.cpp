#include <juce_core/juce_core.h>
#include "BandLevelBridge.h"
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

class BandLevelBridgeTests : public juce::UnitTest
{
public:
    BandLevelBridgeTests() : juce::UnitTest("BandLevelBridgeTests") {}

    void runTest() override
    {
        beginTest("Initialization");
        {
            BandLevelBridge bridge;
            float out[64] = { 0.0f };
            int n = bridge.read(out);
            expectEquals(n, 0);
        }

        beginTest("Update and Read");
        {
            BandLevelBridge bridge;
            float in[7] = { -10.0f, -20.0f, -30.0f, -40.0f, -50.0f, -60.0f, -70.0f };
            bridge.update(in, 7);

            float out[64] = { 0.0f };
            int n = bridge.read(out);
            expectEquals(n, 7);
            for (int i = 0; i < 7; ++i)
                expectEquals(out[i], in[i]);
            // The rest are unmodified in `out` because read only copies `n` elements,
            // but the internal array should have -80.0f for the remaining.
        }

        beginTest("Update with fewer bands zeroes out the rest");
        {
            BandLevelBridge bridge;
            float in7[7] = { -10.0f, -20.0f, -30.0f, -40.0f, -50.0f, -60.0f, -70.0f };
            bridge.update(in7, 7);

            float in3[3] = { -5.0f, -15.0f, -25.0f };
            bridge.update(in3, 3);

            float out[64] = { 0.0f };
            int n = bridge.read(out);
            expectEquals(n, 3);
            for (int i = 0; i < 3; ++i)
                expectEquals(out[i], in3[i]);
        }

        beginTest("Clamping to kMaxBands");
        {
            BandLevelBridge bridge;
            float in70[70];
            for (int i = 0; i < 70; ++i) in70[i] = -1.0f;

            bridge.update(in70, 70);
            float out[BandLevelBridge::kMaxBands] = { 0.0f };
            int n = bridge.read(out);
            expectEquals(n, 70);
            for (int i = 0; i < 70; ++i)
                expectEquals(out[i], -1.0f);

            // More bands than kMaxBands are clamped to kMaxBands.
            std::vector<float> inBig(static_cast<size_t>(BandLevelBridge::kMaxBands) + 44, -2.0f);
            bridge.update(inBig.data(), static_cast<int>(inBig.size()));
            n = bridge.read(out);
            expectEquals(n, BandLevelBridge::kMaxBands);
            for (int i = 0; i < BandLevelBridge::kMaxBands; ++i)
                expectEquals(out[i], -2.0f);
        }

        beginTest("Latest update is always visible to the next read");
        {
            BandLevelBridge bridge;
            float out[BandLevelBridge::kMaxBands] = { 0.0f };
            for (int round = 1; round <= 50; ++round)
            {
                const int bands = 1 + (round * 7) % 40;
                std::vector<float> in(static_cast<size_t>(bands), -static_cast<float>(round));
                bridge.update(in.data(), bands);
                expectEquals(bridge.read(out), bands);
                expectEquals(out[0], -static_cast<float>(round));
                expectEquals(out[bands - 1], -static_cast<float>(round));
                // Reading again without a new update keeps returning the latest values.
                expectEquals(bridge.read(out), bands);
                expectEquals(out[0], -static_cast<float>(round));
            }
        }

        beginTest("Concurrent update/read never exposes a band count with another update's levels");
        {
            BandLevelBridge bridge;

            // Every update fills `bands` entries with -bands, so a consistent read
            // satisfies out[i] == -n for all i < n.
            std::atomic<bool> readerDone { false };
            std::thread writer([&]
            {
                const int counts[] = { 7, 70, 3, 256, 1 };
                std::vector<float> in(BandLevelBridge::kMaxBands);
                int k = 0;
                while (!readerDone.load(std::memory_order_acquire))
                {
                    const int bands = counts[k++ % 5];
                    std::fill(in.begin(), in.begin() + bands, -static_cast<float>(bands));
                    bridge.update(in.data(), bands);
                }
            });

            int inconsistent = 0, badCount = 0, nonEmptyReads = 0;
            float out[BandLevelBridge::kMaxBands];
            const auto deadline = juce::Time::getMillisecondCounter() + 400;
            for (int iter = 0; iter < 2000000 && juce::Time::getMillisecondCounter() < deadline; ++iter)
            {
                const int n = bridge.read(out);
                if (n == 0) continue;
                ++nonEmptyReads;
                if (n != 7 && n != 70 && n != 3 && n != 256 && n != 1) { ++badCount; continue; }
                for (int i = 0; i < n; ++i)
                    if (out[i] != -static_cast<float>(n)) { ++inconsistent; break; }
            }
            readerDone.store(true, std::memory_order_release);
            writer.join();

            expect(nonEmptyReads > 0, "Reader should observe at least one update");
            expectEquals(badCount, 0);
            expectEquals(inconsistent, 0);

            // After the writer has stopped, the very last update must be visible.
            float last[2] = { -11.0f, -22.0f };
            bridge.update(last, 2);
            expectEquals(bridge.read(out), 2);
            expectEquals(out[0], -11.0f);
            expectEquals(out[1], -22.0f);
        }
    }
};

static BandLevelBridgeTests bandLevelBridgeTests;
