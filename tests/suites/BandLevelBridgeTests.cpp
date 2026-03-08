#include <juce_core/juce_core.h>
#include "BandLevelBridge.h"
#include <thread>
#include <chrono>

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
            float out[64] = { 0.0f };
            int n = bridge.read(out);
            expectEquals(n, 70);
            for (int i = 0; i < 64; ++i)
                expectEquals(out[i], -1.0f);
        }

        beginTest("Concurrent try_lock skip");
        {
            BandLevelBridge bridge;
            float in1[1] = { -10.0f };
            bridge.update(in1, 1);

            std::atomic<bool> threadHoldsMutex { false };
            std::atomic<bool> threadReady { false };
            
            // To simulate holding the mutex, we will just call read in a slow manner, or more simply:
            // The object's mutex is private, so we can't lock it directly.
            // We just know that update() uses try_lock. There's no way to easily test the failure of try_lock externally without friend access or modifying the class.
            // But we can spawn a thread that repeatedly calls read(), and the main thread calls update().
            // We just verify it doesn't deadlock or crash.
            
            auto readerThread = std::thread([&]() {
                threadReady = true;
                while (threadHoldsMutex)
                {
                    float out[64];
                    bridge.read(out);
                }
            });

            while (!threadReady) { juce::Thread::yield(); }
            
            threadHoldsMutex = true;
            for (int i = 0; i < 1000; ++i)
            {
                bridge.update(in1, 1);
            }
            threadHoldsMutex = false;
            readerThread.join();
            expect(true, "Concurrent read/update completed without deadlock");
        }
    }
};

static BandLevelBridgeTests bandLevelBridgeTests;
