/**
 * @file LatencyBypassTests.cpp
 * @brief Unit tests for the shared LatencyBypass helper and its latency bound.
 */

#include "../../sample_plugins/common/LatencyBypass.h"

#include "../util/TestUtils.h"

#include <dtcwpt/dtcwpt_processor.h>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <vector>

class LatencyBypassTests : public juce::UnitTest
{
public:
    LatencyBypassTests() : juce::UnitTest ("LatencyBypassTests") {}

    void runTest() override
    {
        beginTest ("Latency bound covers every supported topology and block size");
        testLatencyBoundCoversSupportedTopologies();

        beginTest ("Latency bound is not wasteful for the deepest wavelet tree");
        testLatencyBoundIsTight();

        beginTest ("Delay line is exact and keeps its capacity when the delay changes");
        testDelayIsExactAndCapacityIsStable();

        beginTest ("Cross-fade is linear over 10 ms and ends exactly on the target");
        testCrossFadeShape();

        beginTest ("First block after prepare applies the bypass state without a fade");
        testFirstBlockSnaps();

        beginTest ("Capture and mix are allocation-free");
        testCaptureAndMixAreAllocationFree();
    }

private:
    static constexpr double kSampleRate = TestUtils::TestConfig::SAMPLE_RATE;

    static int engineLatency (const std::vector<std::string>& destinations, int maxDepth, int blockSize)
    {
        dtcwpt::TopologyConfig config;
        config.destinations = destinations;
        config.maxDepth = maxDepth;

        dtcwpt::DTCWPTProcessor engine;
        engine.prepareToPlay (kSampleRate, blockSize, config, 1);
        return engine.getLatency();
    }

    void testLatencyBoundCoversSupportedTopologies()
    {
        constexpr std::array<int, 6> blockSizes { 1, 32, 256, 512, 4096, 5000 };
        const int maxDepth = dtcwpt::topology_limits::kSupportedMaxDepth;

        for (const int blockSize : blockSizes)
        {
            const int bound = latency_bypass::maxEngineLatencySamples (maxDepth, blockSize);

            for (int depth = 1; depth <= maxDepth; ++depth)
            {
                const int latency = engineLatency (TestUtils::getWaveletTreeDestinations (depth), maxDepth, blockSize);
                expect (latency <= bound,
                        "Wavelet tree depth " + juce::String (depth) + ", block " + juce::String (blockSize)
                            + ": latency " + juce::String (latency) + " exceeds bound " + juce::String (bound));
            }

            for (int depth = 1; depth <= 7; ++depth)
            {
                const int latency = engineLatency (TestUtils::getFullPacketDestinations (depth), maxDepth, blockSize);
                expect (latency <= bound,
                        "Full packet depth " + juce::String (depth) + ", block " + juce::String (blockSize)
                            + ": latency " + juce::String (latency) + " exceeds bound " + juce::String (bound));
            }

            for (unsigned int seed = 1; seed <= 4; ++seed)
            {
                const int latency = engineLatency (TestUtils::getRandomPacketDestinations (maxDepth, seed), maxDepth, blockSize);
                expect (latency <= bound,
                        "Random topology seed " + juce::String (static_cast<int> (seed)) + ", block "
                            + juce::String (blockSize) + ": latency " + juce::String (latency)
                            + " exceeds bound " + juce::String (bound));
            }
        }

        // The Gate caps the tree at depth 8 and sizes its delay line with that depth.
        for (const int blockSize : blockSizes)
        {
            const int bound = latency_bypass::maxEngineLatencySamples (8, blockSize);
            const int latency = engineLatency (TestUtils::getWaveletTreeDestinations (8), 8, blockSize);
            expect (latency <= bound,
                    "Gate depth 8, block " + juce::String (blockSize) + ": latency " + juce::String (latency)
                        + " exceeds bound " + juce::String (bound));
        }
    }

    void testLatencyBoundIsTight()
    {
        const int maxDepth = dtcwpt::topology_limits::kSupportedMaxDepth;

        for (const int blockSize : { 32, 256, 4096 })
        {
            const int bound = latency_bypass::maxEngineLatencySamples (maxDepth, blockSize);
            const int latency = engineLatency (TestUtils::getWaveletTreeDestinations (maxDepth), maxDepth, blockSize);

            // The deepest wavelet tree reaches the worst path delay, so only the block-size
            // rounding (less than 2^depth) separates the bound from the real latency.
            expect (bound - latency < (1 << maxDepth),
                    "Block " + juce::String (blockSize) + ": bound " + juce::String (bound)
                        + " should be within 2^depth of the real latency " + juce::String (latency));
        }
    }

    void testDelayIsExactAndCapacityIsStable()
    {
        LatencyBypass bypass;
        bypass.prepare (kSampleRate, 64, 2, 300);
        bypass.setBypassed (true);
        bypass.setDelaySamples (100);

        const int capacity = bypass.getCapacitySamples();
        expectEquals (capacity, 300, "Capacity should be the prepared maximum delay");

        juce::AudioBuffer<double> buffer (2, 64);
        int counter = 0;
        int errors = 0;
        int delay = 100;

        for (int block = 0; block < 12; ++block)
        {
            if (block == 6)
            {
                delay = 40;
                bypass.setDelaySamples (delay);
            }

            for (int i = 0; i < 64; ++i)
            {
                buffer.setSample (0, i, static_cast<double> (counter + i + 1));
                buffer.setSample (1, i, -static_cast<double> (counter + i + 1));
            }

            bypass.captureDry (buffer);
            bypass.mixInto (buffer);

            for (int i = 0; i < 64; ++i)
            {
                const int source = counter + i - delay;
                const double expected = source >= 0 ? static_cast<double> (source + 1) : 0.0;
                if (! (juce::exactlyEqual (buffer.getSample (0, i), expected)
                       && juce::exactlyEqual (buffer.getSample (1, i), -expected)))
                    ++errors;
            }

            counter += 64;
        }

        expectEquals (errors, 0, "Bypassed output should be the input delayed by the configured samples");
        expectEquals (bypass.getCapacitySamples(), capacity, "Changing the delay must not change the capacity");
    }

    void testCrossFadeShape()
    {
        LatencyBypass bypass;
        bypass.prepare (kSampleRate, 1024, 2, 64);
        bypass.setDelaySamples (0);

        juce::AudioBuffer<double> buffer (2, 1024);

        // Block 1: live. Wet = 1, dry (input) = 0.
        buffer.clear();
        bypass.setBypassed (false);
        bypass.captureDry (buffer);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 1.0, 1024);
        bypass.mixInto (buffer);
        expectEquals (buffer.getSample (0, 1023), 1.0, "Live blocks leave the wet signal untouched");

        // Block 2: bypass requested. Input is zero (dry = 0), wet = 1: the output is 1 - g.
        buffer.clear();
        bypass.setBypassed (true);
        bypass.captureDry (buffer);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 1.0, 1024);
        bypass.mixInto (buffer);

        const int fadeSamples = static_cast<int> (kSampleRate * LatencyBypass::kFadeSeconds);
        const double step = 1.0 / static_cast<double> (fadeSamples);
        bool linear = true;
        bool channelsMatch = true;

        double previous = 1.0;
        for (int i = 0; i < fadeSamples; ++i)
        {
            const double value = buffer.getSample (0, i);
            if (std::abs ((previous - value) - step) > 1.0e-12)
                linear = false;
            if (! juce::exactlyEqual (value, buffer.getSample (1, i)))
                channelsMatch = false;
            previous = value;
        }

        expect (linear, "The fade should decrease by 1 / (10 ms in samples) per sample");
        expect (channelsMatch, "Both channels should share one fade");
        expectEquals (buffer.getSample (0, fadeSamples - 1), 0.0, "The fade should end exactly on the dry signal");
        expectEquals (buffer.getSample (0, 1023), 0.0, "After the fade the output is exactly the dry signal");
    }

    void testFirstBlockSnaps()
    {
        LatencyBypass bypass;
        bypass.prepare (kSampleRate, 256, 2, 64);
        bypass.setDelaySamples (0);
        bypass.setBypassed (true);

        juce::AudioBuffer<double> buffer (2, 256);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.25, 256);

        bypass.captureDry (buffer);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 1.0, 256);
        bypass.mixInto (buffer);

        expectEquals (buffer.getSample (0, 0), 0.25, "A bypass requested before the first block takes effect immediately");
        expectEquals (buffer.getSample (1, 255), 0.25, "A bypass requested before the first block stays fully applied");
    }

    void testCaptureAndMixAreAllocationFree()
    {
        LatencyBypass bypass;
        bypass.prepare (kSampleRate, 128, 2, 4000);
        bypass.setDelaySamples (1500);

        juce::AudioBuffer<double> buffer (2, 128);
        std::size_t allocations = 0;

        for (int iteration = 0; iteration < 12; ++iteration)
        {
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.5, 128);

            TestUtils::AllocationCounter counter;
            counter.begin();

            bypass.setBypassed (iteration >= 3 && iteration < 8);
            bypass.setDelaySamples (iteration < 6 ? 1500 : 3999);
            bypass.captureDry (buffer);
            bypass.mixInto (buffer);

            counter.end();
            allocations += counter.getCount();
        }

        expectEquals (static_cast<int> (allocations), 0, "captureDry / mixInto / setDelaySamples must not allocate");
    }
};

static LatencyBypassTests latencyBypassTests;
