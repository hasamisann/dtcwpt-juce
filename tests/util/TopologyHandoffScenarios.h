/**
 * @file TopologyHandoffScenarios.h
 * @brief Plugin-API level scenarios for topology changes, shared by Morph and Gate.
 *
 * The scenarios only use the public AudioProcessor surface plus the topology API
 * (requestTopologyChange / getCommittedTopology / getTopologyGeneration), so the same
 * test body runs against both plugins. Each plugin has its own translation unit that
 * includes its processor header and instantiates the template (Morph and Gate define
 * classes with the same names, so both cannot be included in one translation unit).
 *
 * Covered issue symptoms:
 *  - #3: engine rebuild, latency notification and ValueTree writes must not happen on
 *        the audio thread, and a topology switch must not click.
 *  - #4: audio thread and state save/restore/GUI reads must not race.
 *  - #5: no lost update, no stolen request, state restore is applied to the engine.
 */

#pragma once

#include "TestUtils.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

template <typename Processor>
class TopologyHandoffScenarios : public juce::UnitTest
{
public:
    explicit TopologyHandoffScenarios (const juce::String& suiteName) : juce::UnitTest (suiteName) {}

    void runTest() override
    {
        beginTest ("#3: processBlock after a topology request does not allocate");
        testProcessBlockAfterRequestDoesNotAllocate();

        beginTest ("#3: latency is updated when the request is made, not on the audio thread");
        testLatencyIsUpdatedAtRequestTime();

        beginTest ("#3/#4: processBlock does not touch the APVTS ValueTree");
        testProcessBlockDoesNotNotifyValueTreeListeners();

        beginTest ("#3: topology switch does not click (block 512, fade inside one block)");
        testSwitchDoesNotClick (512);

        beginTest ("#3: topology switch does not click (block 64, fade spans blocks)");
        testSwitchDoesNotClick (64);

        beginTest ("#5: saved state reflects the new topology as soon as it is requested");
        testSavedStateReflectsRequestImmediately();

        beginTest ("#5: topology generation advances only on committed changes");
        testGenerationAdvancesOnlyOnCommittedChanges();

        beginTest ("#5: A then B requested before processBlock applies B");
        testLatestRequestWins();

        beginTest ("#5: request made right after the audio thread took the previous one is applied");
        testRequestAfterAcquireIsApplied();

        beginTest ("#5: requests arriving while a switch is in progress are not lost");
        testRequestDuringSwitchIsNotLost (64);

        beginTest ("#5: state restore followed by GUI-style reads is still applied to the engine");
        testStateRestoreIsNotStolenByReaders();

        beginTest ("#5: many requests with and without audio stay bounded and apply the last one");
        testManyRequestsStayBounded();

        beginTest ("#4: audio thread vs state save/restore/GUI reads (stress)");
        testConcurrentStateAccess();
    }

private:
    static constexpr int kDefaultBlockSize = 512;

    static const std::vector<std::string>& defaultTopology()
    {
        static const std::vector<std::string> topology { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" };
        return topology;
    }

    static std::unique_ptr<Processor> createPrepared (int blockSize = kDefaultBlockSize)
    {
        auto processor = std::make_unique<Processor>();
        processor->prepareToPlay (TestUtils::TestConfig::SAMPLE_RATE, blockSize);
        return processor;
    }

    /** Streams a continuous 440 Hz sine into a processor and records channel 0 of the output. */
    struct SineRunner
    {
        explicit SineRunner (int blockSizeIn) : blockSize (blockSizeIn) {}

        void run (Processor& processor, int numBlocks)
        {
            juce::MidiBuffer midi;

            for (int block = 0; block < numBlocks; ++block)
            {
                juce::AudioBuffer<float> buffer (2, blockSize);

                for (int i = 0; i < blockSize; ++i)
                {
                    const float sample = static_cast<float> (0.5 * std::sin (phase));
                    phase += juce::MathConstants<double>::twoPi * 440.0 / TestUtils::TestConfig::SAMPLE_RATE;
                    buffer.setSample (0, i, sample);
                    buffer.setSample (1, i, sample);
                }

                processor.processBlock (buffer, midi);

                for (int i = 0; i < blockSize; ++i)
                    output.push_back (buffer.getSample (0, i));
            }
        }

        double rmsOfLastBlock() const
        {
            double sum = 0.0;
            for (int i = 0; i < blockSize; ++i)
            {
                const double v = static_cast<double> (output[output.size() - 1 - static_cast<std::size_t> (i)]);
                sum += v * v;
            }
            return std::sqrt (sum / static_cast<double> (blockSize));
        }

        int blockSize;
        double phase = 0.0;
        std::vector<float> output;
    };

    static void processSilence (Processor& processor, int numBlocks, int blockSize = kDefaultBlockSize)
    {
        juce::MidiBuffer midi;
        for (int block = 0; block < numBlocks; ++block)
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            buffer.clear();
            processor.processBlock (buffer, midi);
        }
    }

    /** Latency the plugin reports once `destinations` is the active topology. */
    static int latencyOf (const std::vector<std::string>& destinations, int blockSize = kDefaultBlockSize)
    {
        auto reference = createPrepared (blockSize);
        reference->requestTopologyChange (destinations);
        processSilence (*reference, 2, blockSize);
        return reference->getLatencySamples();
    }

    static juce::MemoryBlock makeStateWithTopology (const std::vector<std::string>& destinations)
    {
        Processor source;
        juce::ValueTree state = source.getAPVTS().copyState();
        state.setProperty ("topology", juce::String (joinDestinations (destinations)), nullptr);

        juce::MemoryBlock block;
        juce::MemoryOutputStream stream (block, true);
        state.writeToStream (stream);
        return block;
    }

    static std::string joinDestinations (const std::vector<std::string>& destinations)
    {
        std::string joined;
        for (std::size_t i = 0; i < destinations.size(); ++i)
        {
            if (i > 0)
                joined += ",";
            joined += destinations[i];
        }
        return joined;
    }

    static juce::String savedTopologyProperty (Processor& processor)
    {
        juce::MemoryBlock block;
        processor.getStateInformation (block);
        const auto state = juce::ValueTree::readFromData (block.getData(), block.getSize());
        return state.getProperty ("topology").toString();
    }

    //==========================================================================
    // #3
    //==========================================================================

    void testProcessBlockAfterRequestDoesNotAllocate()
    {
        auto processor = createPrepared();
        processSilence (*processor, 2);

        processor->requestTopologyChange (TestUtils::getFullPacketDestinations (5));

        juce::AudioBuffer<float> buffer (2, kDefaultBlockSize);
        buffer.clear();
        juce::MidiBuffer midi;

        TestUtils::AllocationCounter counter;
        counter.begin();
        processor->processBlock (buffer, midi);
        counter.end();

        expectEquals (static_cast<int> (counter.getCount()), 0,
                      "processBlock right after a topology request must not allocate (engine is built off the audio thread)");
    }

    void testLatencyIsUpdatedAtRequestTime()
    {
        const auto target = TestUtils::getFullPacketDestinations (5);
        const int targetLatency = latencyOf (target);

        auto processor = createPrepared();
        const int initialLatency = processor->getLatencySamples();
        expect (initialLatency != targetLatency, "Sanity: the target topology must have a different latency");

        processor->requestTopologyChange (target);

        expectEquals (processor->getLatencySamples(), targetLatency,
                      "The new latency must be reported when the request is made, before any processBlock");
    }

    struct CountingListener : public juce::ValueTree::Listener
    {
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { ++count; }
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { ++count; }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { ++count; }
        void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { ++count; }
        void valueTreeRedirected (juce::ValueTree&) override { ++count; }

        int count = 0;
    };

    void testProcessBlockDoesNotNotifyValueTreeListeners()
    {
        auto processor = createPrepared();
        processSilence (*processor, 2);

        processor->requestTopologyChange (TestUtils::getFullPacketDestinations (4));

        CountingListener listener;
        processor->getAPVTS().state.addListener (&listener);
        const int before = listener.count;
        processSilence (*processor, 2);
        const int duringProcessBlock = listener.count - before;
        processor->getAPVTS().state.removeListener (&listener);

        expectEquals (duringProcessBlock, 0,
                      "processBlock must not write to the APVTS ValueTree (listeners would fire on the audio thread)");
    }

    void testSwitchDoesNotClick (int blockSize)
    {
        const auto target = TestUtils::getFullPacketDestinations (4);
        const int targetLatency = latencyOf (target, blockSize);

        auto processor = createPrepared (blockSize);
        SineRunner sine (blockSize);

        const int warmupBlocks = processor->getLatencySamples() / blockSize + 16;
        sine.run (*processor, warmupBlocks);
        expect (sine.rmsOfLastBlock() > 0.05, "Sanity: the settled signal must be audible before the switch");

        const std::size_t switchIndex = sine.output.size();
        processor->requestTopologyChange (target);
        sine.run (*processor, (targetLatency + 8192) / blockSize + 1);
        expect (sine.rmsOfLastBlock() > 0.05, "Sanity: the signal must be audible again after the switch");

        float maxStep = 0.0f;
        for (std::size_t i = switchIndex; i < sine.output.size(); ++i)
            maxStep = std::max (maxStep, std::abs (sine.output[i] - sine.output[i - 1]));

        logMessage ("block " + juce::String (blockSize) + ": max adjacent sample step across the switch = "
                    + juce::String (maxStep, 4));

        // A 0.5 amplitude 440 Hz sine moves by at most ~0.032 per sample. A hard cut or a hard start
        // of the new engine produces steps of the order of the signal amplitude.
        expect (maxStep < 0.08f,
                "Switching topology must not introduce a click; max adjacent sample step was "
                    + juce::String (maxStep, 4));
    }

    //==========================================================================
    // #5
    //==========================================================================

    void testSavedStateReflectsRequestImmediately()
    {
        const auto target = TestUtils::getFullPacketDestinations (3);
        auto processor = createPrepared();

        processor->requestTopologyChange (target);

        expectEquals (savedTopologyProperty (*processor), juce::String (joinDestinations (target)),
                      "getStateInformation right after a request must already contain the new topology");
        expect (processor->getCommittedTopology() == target,
                "getCommittedTopology right after a request must return the new topology");
    }

    void testGenerationAdvancesOnlyOnCommittedChanges()
    {
        auto processor = createPrepared();
        const auto generationBefore = processor->getTopologyGeneration();

        const auto valid = TestUtils::getFullPacketDestinations (3);
        processor->requestTopologyChange (valid);
        const auto generationAfterValid = processor->getTopologyGeneration();
        expect (generationAfterValid != generationBefore, "A committed change must advance the generation");

        processor->requestTopologyChange ({ "L", "HL" });
        expectEquals (static_cast<int> (processor->getTopologyGeneration()), static_cast<int> (generationAfterValid),
                      "A rejected request must not advance the generation");
        expect (processor->getCommittedTopology() == valid, "A rejected request must keep the committed topology");
    }

    void testLatestRequestWins()
    {
        const auto a = TestUtils::getFullPacketDestinations (3);
        const auto b = TestUtils::getFullPacketDestinations (5);
        const int latencyA = latencyOf (a);
        const int latencyB = latencyOf (b);
        expect (latencyA != latencyB, "Sanity: A and B must have different latencies");

        auto processor = createPrepared();
        processSilence (*processor, 2);
        processor->requestTopologyChange (a);
        processor->requestTopologyChange (b);
        processSilence (*processor, 4);

        expect (processor->getCommittedTopology() == b, "B must be the committed topology");
        expectEquals (processor->getLatencySamples(), latencyB, "Reported latency must follow B");
        expectEquals (processor->getAnalyzerInputDelaySamplesForTest(), latencyB,
                      "The engine running after processBlock must be B's (analyzer delay follows the active engine)");
    }

    void testRequestAfterAcquireIsApplied()
    {
        const auto a = TestUtils::getFullPacketDestinations (3);
        const auto b = TestUtils::getFullPacketDestinations (5);
        const int latencyB = latencyOf (b);

        auto processor = createPrepared();
        processSilence (*processor, 2);

        processor->requestTopologyChange (a);
        processSilence (*processor, 1);   // the audio thread takes A
        processor->requestTopologyChange (b);
        processSilence (*processor, 4);   // ... and the next callbacks must pick up B

        expect (processor->getCommittedTopology() == b, "B must be the committed topology");
        expectEquals (processor->getAnalyzerInputDelaySamplesForTest(), latencyB,
                      "B must be applied to the engine after A was already taken");
    }

    void testRequestDuringSwitchIsNotLost (int blockSize)
    {
        const auto a = TestUtils::getFullPacketDestinations (3);
        const auto b = TestUtils::getFullPacketDestinations (4);
        const auto c = TestUtils::getFullPacketDestinations (5);
        const int latencyC = latencyOf (c, blockSize);

        auto processor = createPrepared (blockSize);
        SineRunner sine (blockSize);
        sine.run (*processor, 40);

        // Requests land in the middle of the cross-fade of the previous one (fade is ~441 samples, 7 blocks of 64).
        processor->requestTopologyChange (a);
        sine.run (*processor, 2);
        processor->requestTopologyChange (b);
        sine.run (*processor, 1);
        processor->requestTopologyChange (c);
        sine.run (*processor, 64);

        expect (processor->getCommittedTopology() == c, "The last request must be the committed topology");
        expectEquals (processor->getAnalyzerInputDelaySamplesForTest(), latencyC,
                      "The last request must end up on the engine even if it arrived during a switch");
    }

    void testStateRestoreIsNotStolenByReaders()
    {
        const auto restored = TestUtils::getFullPacketDestinations (5);
        const int restoredLatency = latencyOf (restored);
        const auto state = makeStateWithTopology (restored);

        auto processor = createPrepared();
        processSilence (*processor, 2);

        processor->setStateInformation (state.getData(), static_cast<int> (state.getSize()));

        // What an editor does when it opens: read the topology (several times).
        for (int i = 0; i < 3; ++i)
            expect (processor->getCommittedTopology() == restored, "Readers must see the restored topology");

        processSilence (*processor, 4);

        expectEquals (processor->getAnalyzerInputDelaySamplesForTest(), restoredLatency,
                      "The engine must run the restored topology even though readers looked at it first");
        expectEquals (processor->getLatencySamples(), restoredLatency, "Reported latency must follow the restored topology");
    }

    void testManyRequestsStayBounded()
    {
        const std::vector<std::vector<std::string>> topologies {
            TestUtils::getFullPacketDestinations (2),
            TestUtils::getFullPacketDestinations (3),
            TestUtils::getWaveletTreeDestinations (4),
            defaultTopology()
        };

        auto processor = createPrepared();

        // Phase 1: the audio thread is idle (e.g. transport stopped / no callbacks) while requests keep coming.
        for (int i = 0; i < 40; ++i)
            processor->requestTopologyChange (topologies[static_cast<std::size_t> (i) % topologies.size()]);

        // Phase 2: requests interleaved with callbacks.
        for (int i = 0; i < 60; ++i)
        {
            processor->requestTopologyChange (topologies[static_cast<std::size_t> (i) % topologies.size()]);
            if (i % 3 != 0)
                processSilence (*processor, 1);
        }

        const auto last = topologies[59 % topologies.size()];
        processSilence (*processor, 8);

        expect (processor->getCommittedTopology() == last, "The last request must be the committed topology");
        expectEquals (processor->getAnalyzerInputDelaySamplesForTest(), latencyOf (last),
                      "The last request must be running on the engine");
    }

    //==========================================================================
    // #4
    //==========================================================================

    void testConcurrentStateAccess()
    {
        const std::vector<std::vector<std::string>> topologies {
            TestUtils::getFullPacketDestinations (3),
            TestUtils::getWaveletTreeDestinations (4),
            TestUtils::getFullPacketDestinations (2)
        };

        std::vector<juce::MemoryBlock> states;
        for (const auto& topology : topologies)
            states.push_back (makeStateWithTopology (topology));

        auto processor = createPrepared (256);
        processSilence (*processor, 2, 256);

        std::atomic<bool> stopAudio { false };
        std::atomic<int> unexpectedReads { 0 };
        constexpr int kIterations = 30;

        auto isKnownTopology = [&] (const std::vector<std::string>& candidate)
        {
            if (candidate == defaultTopology())
                return true;

            return std::any_of (topologies.begin(), topologies.end(),
                                [&] (const auto& known) { return known == candidate; });
        };

        std::thread audioThread ([&]
        {
            SineRunner sine (256);
            while (! stopAudio.load())
            {
                sine.output.clear();
                sine.run (*processor, 1);
            }
        });

        std::thread saveThread ([&]
        {
            for (int i = 0; i < kIterations * 4; ++i)
            {
                juce::MemoryBlock block;
                processor->getStateInformation (block);
                if (block.getSize() == 0)
                    ++unexpectedReads;
            }
        });

        std::thread restoreThread ([&]
        {
            for (int i = 0; i < kIterations; ++i)
            {
                const auto& state = states[static_cast<std::size_t> (i) % states.size()];
                processor->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
            }
        });

        std::thread guiThread ([&]
        {
            for (int i = 0; i < kIterations * 4; ++i)
            {
                if (! isKnownTopology (processor->getCommittedTopology()))
                    ++unexpectedReads;

                (void) processor->getTopologyGeneration();

                if (i % 4 == 0)
                    processor->requestTopologyChange (topologies[static_cast<std::size_t> (i / 4) % topologies.size()]);
            }
        });

        saveThread.join();
        restoreThread.join();
        guiThread.join();
        stopAudio.store (true);
        audioThread.join();

        expectEquals (unexpectedReads.load(), 0, "Concurrent readers must only ever observe complete, known topologies");

        // Whatever interleaving happened, the last committed topology must end up on the engine.
        const auto committed = processor->getCommittedTopology();
        processSilence (*processor, 8, 256);
        expectEquals (processor->getAnalyzerInputDelaySamplesForTest(), latencyOf (committed, 256),
                      "After the stress run the engine must match the committed topology");
    }
};
