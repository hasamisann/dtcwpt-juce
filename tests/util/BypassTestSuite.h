/**
 * @file BypassTestSuite.h
 * @brief Shared bypass / latency-compensation tests for the Morph and Gate processors.
 *
 * The two plugin headers define identically named helper types, so they cannot be
 * included in one translation unit. Each plugin therefore instantiates this suite
 * template from its own .cpp with a small traits class:
 *
 *   struct Traits {
 *       static const char* suiteName();
 *       static constexpr int kNumInputChannels;      // channels in the host buffer
 *       static constexpr int kDeepTopologyDepth;     // deepest wavelet tree the plugin accepts
 *       static constexpr float kSineAmplitude;       // test tone level that configureSilentWet() mutes
 *       static void configureSilentWet (Processor&);  // make the active path output silence
 *   };
 */

#pragma once

#include "TestUtils.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

template <class Processor, class Traits>
class BypassTestSuite : public juce::UnitTest
{
public:
    BypassTestSuite() : juce::UnitTest (Traits::suiteName()) {}

    void runTest() override
    {
        beginTest ("Bypass parameter is exposed to the host");
        testBypassParameterIsExposed();

        beginTest ("processBlockBypassed outputs the input delayed by the reported latency");
        testProcessBlockBypassedIsDelayedInput();

        beginTest ("Bypass parameter switches to the delayed dry signal after the fade");
        testBypassParameterSwitchesToDelayedDry();

        beginTest ("Engine keeps running while bypassed and un-bypass returns to the live output");
        testEngineKeepsRunningWhileBypassed();

        beginTest ("Toggling bypass does not produce clicks");
        testToggleHasNoClicks();

        beginTest ("Forced bypass transitions do not produce clicks");
        testForcedBypassTransitionHasNoClicks();

        beginTest ("Dry delay follows the latency after a topology change");
        testDryDelayFollowsTopologyChange();

        beginTest ("Deepest topology latency is matched by the dry delay");
        testDeepestTopologyLatency();

        beginTest ("Bypass paths remain allocation-free");
        testBypassPathsRemainAllocationFree();
    }

private:
    static constexpr int kBlockSize = 512;
    static constexpr double kSampleRate = TestUtils::TestConfig::SAMPLE_RATE;
    // 10 ms fade at 44.1 kHz is 441 samples; leave generous margin.
    static constexpr int kFadeMarginSamples = 1024;

    struct Stereo
    {
        std::vector<float> left;
        std::vector<float> right;
    };

    enum class Call { normal, bypassed };

    static std::unique_ptr<Processor> createPrepared()
    {
        auto processor = std::make_unique<Processor>();
        processor->prepareToPlay (kSampleRate, kBlockSize);
        return processor;
    }

    static Stereo makeNoise (int numSamples)
    {
        juce::Random rng (1234);
        Stereo signal;
        signal.left.resize (static_cast<std::size_t> (numSamples));
        signal.right.resize (static_cast<std::size_t> (numSamples));
        for (int i = 0; i < numSamples; ++i)
        {
            signal.left[static_cast<std::size_t> (i)] = rng.nextFloat() - 0.5f;
            signal.right[static_cast<std::size_t> (i)] = rng.nextFloat() - 0.5f;
        }
        return signal;
    }

    static Stereo makeSine (int numSamples, float amplitude)
    {
        Stereo signal;
        signal.left.resize (static_cast<std::size_t> (numSamples));
        signal.right.resize (static_cast<std::size_t> (numSamples));
        for (int i = 0; i < numSamples; ++i)
        {
            const double t = static_cast<double> (i) / kSampleRate;
            signal.left[static_cast<std::size_t> (i)] =
                amplitude * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * 440.0 * t));
            signal.right[static_cast<std::size_t> (i)] =
                amplitude * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * 523.25 * t));
        }
        return signal;
    }

    static void fillBuffer (juce::AudioBuffer<float>& buffer, const Stereo& input, int offset, int count)
    {
        buffer.clear();
        for (int i = 0; i < count; ++i)
        {
            buffer.setSample (0, i, input.left[static_cast<std::size_t> (offset + i)]);
            buffer.setSample (1, i, input.right[static_cast<std::size_t> (offset + i)]);
        }
    }

    /**
     * Runs `input` through the processor block by block. `beforeBlock` runs before each
     * block (to toggle parameters); `callFor` selects processBlock or processBlockBypassed.
     */
    static Stereo run (Processor& processor,
                       const Stereo& input,
                       const std::function<Call (int)>& callFor,
                       const std::function<void (int)>& beforeBlock = {})
    {
        const int total = static_cast<int> (input.left.size());
        Stereo output;
        output.left.reserve (input.left.size());
        output.right.reserve (input.right.size());

        juce::MidiBuffer midi;
        int blockIndex = 0;
        for (int offset = 0; offset < total; offset += kBlockSize, ++blockIndex)
        {
            const int count = std::min (kBlockSize, total - offset);
            juce::AudioBuffer<float> buffer (Traits::kNumInputChannels, count);
            fillBuffer (buffer, input, offset, count);

            if (beforeBlock)
                beforeBlock (blockIndex);

            if (callFor (blockIndex) == Call::bypassed)
                processor.processBlockBypassed (buffer, midi);
            else
                processor.processBlock (buffer, midi);

            for (int i = 0; i < count; ++i)
            {
                output.left.push_back (buffer.getSample (0, i));
                output.right.push_back (buffer.getSample (1, i));
            }
        }

        return output;
    }

    static void setBypassParameter (Processor& processor, bool bypassed)
    {
        if (auto* parameter = processor.getBypassParameter())
            parameter->setValueNotifyingHost (bypassed ? 1.0f : 0.0f);
    }

    /** Expected dry sample: input delayed by `latency`, zero before the start. */
    static float delayedInput (const std::vector<float>& input, int index, int latency)
    {
        const int source = index - latency;
        return source >= 0 ? input[static_cast<std::size_t> (source)] : 0.0f;
    }

    /** Counts mismatches against the delayed input from `startIndex`; returns the first bad index or -1. */
    int findFirstDelayedMismatch (const Stereo& input, const Stereo& output, int latency, int startIndex)
    {
        const int total = static_cast<int> (output.left.size());
        for (int i = startIndex; i < total; ++i)
        {
            if (! sameSample (output.left[static_cast<std::size_t> (i)], delayedInput (input.left, i, latency))
                || ! sameSample (output.right[static_cast<std::size_t> (i)], delayedInput (input.right, i, latency)))
                return i;
        }
        return -1;
    }

    static bool sameSample (float a, float b) noexcept { return ! (a < b) && ! (a > b); }

    static float maxAdjacentDifference (const Stereo& signal, int startIndex, int endIndex)
    {
        float maxDifference = 0.0f;
        for (int i = std::max (1, startIndex); i < endIndex; ++i)
        {
            const auto index = static_cast<std::size_t> (i);
            maxDifference = std::max (maxDifference, std::abs (signal.left[index] - signal.left[index - 1]));
            maxDifference = std::max (maxDifference, std::abs (signal.right[index] - signal.right[index - 1]));
        }
        return maxDifference;
    }

    static float peak (const Stereo& signal, int startIndex, int endIndex)
    {
        float result = 0.0f;
        for (int i = startIndex; i < endIndex; ++i)
        {
            result = std::max (result, std::abs (signal.left[static_cast<std::size_t> (i)]));
            result = std::max (result, std::abs (signal.right[static_cast<std::size_t> (i)]));
        }
        return result;
    }

    //==========================================================================

    void testBypassParameterIsExposed()
    {
        auto processor = createPrepared();
        auto* parameter = processor->getBypassParameter();

        expect (parameter != nullptr, "getBypassParameter() should expose a bypass parameter");
        if (parameter == nullptr)
            return;

        expect (dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr,
                "The bypass parameter should be an AudioParameterBool");

        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
            expectEquals (withId->paramID, juce::String ("bypass"), "Bypass parameter ID should be \"bypass\"");

        expectEquals (parameter->getDefaultValue(), 0.0f, "Bypass should default to off");
        expect (processor->getAPVTS().getParameter ("bypass") == parameter,
                "The bypass parameter should be the APVTS parameter \"bypass\"");
    }

    void testProcessBlockBypassedIsDelayedInput()
    {
        auto processor = createPrepared();
        const int latency = processor->getLatencySamples();
        expect (latency > 0, "Precondition: the plugin reports latency");

        const auto input = makeNoise (kBlockSize * 12);
        const auto output = run (*processor, input, [] (int) { return Call::bypassed; });

        const int mismatch = findFirstDelayedMismatch (input, output, latency, 0);
        expect (mismatch < 0,
                "processBlockBypassed should output the input delayed by getLatencySamples() ("
                    + juce::String (latency) + "); first mismatch at sample " + juce::String (mismatch));
    }

    void testBypassParameterSwitchesToDelayedDry()
    {
        auto processor = createPrepared();
        const int latency = processor->getLatencySamples();
        const auto input = makeNoise (kBlockSize * 24);
        constexpr int kSwitchBlock = 4;

        const auto output = run (*processor, input,
                                 [] (int) { return Call::normal; },
                                 [&] (int block) { if (block == kSwitchBlock) setBypassParameter (*processor, true); });

        expect (processor->getBypassParameter() != nullptr, "Precondition: bypass parameter exists");

        const int firstFullBypass = kSwitchBlock * kBlockSize + kFadeMarginSamples;
        const int mismatch = findFirstDelayedMismatch (input, output, latency, firstFullBypass);
        expect (mismatch < 0,
                "After the fade, bypass should output the delayed input sample for sample; first mismatch at sample "
                    + juce::String (mismatch));
    }

    void testEngineKeepsRunningWhileBypassed()
    {
        auto reference = createPrepared();
        auto bypassedThenRestored = createPrepared();
        const auto input = makeNoise (kBlockSize * 40);
        constexpr int kBypassOnBlock = 6;
        constexpr int kBypassOffBlock = 20;

        const auto referenceOutput = run (*reference, input, [] (int) { return Call::normal; });
        const auto output = run (*bypassedThenRestored, input,
                                 [] (int) { return Call::normal; },
                                 [&] (int block)
                                 {
                                     if (block == kBypassOnBlock)  setBypassParameter (*bypassedThenRestored, true);
                                     if (block == kBypassOffBlock) setBypassParameter (*bypassedThenRestored, false);
                                 });

        expect (bypassedThenRestored->getBypassParameter() != nullptr, "Precondition: bypass parameter exists");

        // The engine kept processing during the bypass, so once the fade-back is done the
        // output must be identical to a processor that was never bypassed.
        const int start = kBypassOffBlock * kBlockSize + kFadeMarginSamples;
        int mismatch = -1;
        for (int i = start; i < static_cast<int> (output.left.size()) && mismatch < 0; ++i)
        {
            const auto index = static_cast<std::size_t> (i);
            if (! sameSample (output.left[index], referenceOutput.left[index])
                || ! sameSample (output.right[index], referenceOutput.right[index]))
                mismatch = i;
        }

        expect (mismatch < 0,
                "After un-bypass the output should match a never-bypassed processor; first mismatch at sample "
                    + juce::String (mismatch));
    }

    void testToggleHasNoClicks()
    {
        auto processor = createPrepared();
        Traits::configureSilentWet (*processor);
        const int latency = processor->getLatencySamples();

        const int total = kBlockSize * 60;
        const auto input = makeSine (total, Traits::kSineAmplitude);
        const int onBlock = (latency + 4096) / kBlockSize + 1;
        const int offBlock = onBlock + 20;

        const auto output = run (*processor, input,
                                 [] (int) { return Call::normal; },
                                 [&] (int block)
                                 {
                                     if (block == onBlock)  setBypassParameter (*processor, true);
                                     if (block == offBlock) setBypassParameter (*processor, false);
                                 });

        expect (processor->getBypassParameter() != nullptr, "Precondition: bypass parameter exists");

        const int warmup = onBlock * kBlockSize;
        expect (peak (output, warmup - kBlockSize, warmup) < 1.0e-3f,
                "Precondition: the live path is silent so that bypass switching is audible as a jump; peak = "
                    + juce::String (peak (output, warmup - kBlockSize, warmup)));

        const int bypassedPeakEnd = offBlock * kBlockSize;
        expect (peak (output, warmup + kFadeMarginSamples, bypassedPeakEnd) > 0.8f * Traits::kSineAmplitude,
                "Precondition: bypass outputs the dry sine");

        // A sine at 523.25 Hz moves by at most 0.075 * amplitude per sample and a fade over
        // 441 samples adds about amplitude / 441, so 0.12 * amplitude leaves margin, while a
        // hard switch (a jump of up to the full amplitude) fails.
        const float maxStep = maxAdjacentDifference (output, warmup - 8, total);
        expect (maxStep < 0.12f * Traits::kSineAmplitude,
                "Toggling bypass should not create discontinuities; max adjacent difference = "
                    + juce::String (maxStep));
    }

    void testForcedBypassTransitionHasNoClicks()
    {
        auto processor = createPrepared();
        Traits::configureSilentWet (*processor);
        const int latency = processor->getLatencySamples();

        const int total = kBlockSize * 60;
        const auto input = makeSine (total, Traits::kSineAmplitude);
        const int forceStart = (latency + 4096) / kBlockSize + 1;
        const int forceEnd = forceStart + 20;

        const auto output = run (*processor, input,
                                 [&] (int block)
                                 {
                                     return (block >= forceStart && block < forceEnd) ? Call::bypassed : Call::normal;
                                 });

        const int warmup = forceStart * kBlockSize;
        expect (peak (output, warmup - kBlockSize, warmup) < 1.0e-3f,
                "Precondition: the live path is silent so that forced bypass is audible as a jump");

        const float maxStep = maxAdjacentDifference (output, warmup - 8, total);
        expect (maxStep < 0.12f * Traits::kSineAmplitude,
                "processBlockBypassed transitions should not create discontinuities; max adjacent difference = "
                    + juce::String (maxStep));
    }

    void testDryDelayFollowsTopologyChange()
    {
        auto processor = createPrepared();
        const int initialLatency = processor->getLatencySamples();
        const auto newTopology = TestUtils::getFullPacketDestinations (4);

        const auto input = makeNoise (kBlockSize * 16);
        constexpr int kChangeBlock = 8;
        int latencyAfterChange = initialLatency;

        const auto output = run (*processor, input,
                                 [] (int) { return Call::bypassed; },
                                 [&] (int block)
                                 {
                                     if (block == kChangeBlock)
                                     {
                                         expect (processor->applyTopologyCandidateForTest (newTopology),
                                                 "Topology change should be accepted");
                                         latencyAfterChange = processor->getLatencySamples();
                                     }
                                 });

        expect (latencyAfterChange != initialLatency,
                "Precondition: the new topology changes the reported latency");

        const int mismatchBefore = findFirstDelayedMismatch (
            input, Stereo { std::vector<float> (output.left.begin(), output.left.begin() + kChangeBlock * kBlockSize),
                            std::vector<float> (output.right.begin(), output.right.begin() + kChangeBlock * kBlockSize) },
            initialLatency, 0);
        expect (mismatchBefore < 0, "Before the change the dry delay should equal the initial latency");

        const int mismatchAfter = findFirstDelayedMismatch (input, output, latencyAfterChange, kChangeBlock * kBlockSize);
        expect (mismatchAfter < 0,
                "After the change the dry delay should equal the new latency ("
                    + juce::String (latencyAfterChange) + "); first mismatch at sample "
                    + juce::String (mismatchAfter));
    }

    void testDeepestTopologyLatency()
    {
        auto processor = createPrepared();
        expect (processor->applyTopologyCandidateForTest (TestUtils::getWaveletTreeDestinations (Traits::kDeepTopologyDepth)),
                "Deepest wavelet tree should be accepted");

        const int latency = processor->getLatencySamples();
        const int blocks = (latency + kFadeMarginSamples) / kBlockSize + 4;
        const auto input = makeNoise (kBlockSize * blocks);
        const auto output = run (*processor, input, [] (int) { return Call::bypassed; });

        const int mismatch = findFirstDelayedMismatch (input, output, latency, 0);
        expect (mismatch < 0,
                "Bypass should match the delayed input for the deepest topology (latency "
                    + juce::String (latency) + "); first mismatch at sample " + juce::String (mismatch));
    }

    void testBypassPathsRemainAllocationFree()
    {
        auto processor = createPrepared();
        const auto input = makeNoise (kBlockSize);
        juce::AudioBuffer<float> buffer (Traits::kNumInputChannels, kBlockSize);
        juce::MidiBuffer midi;

        std::size_t allocations = 0;

        for (int iteration = 0; iteration < 6; ++iteration)
        {
            fillBuffer (buffer, input, 0, kBlockSize);

            // Parameter changes come from the host / message thread, so they stay outside the counted region.
            if (iteration == 2) setBypassParameter (*processor, true);
            if (iteration == 4) setBypassParameter (*processor, false);

            TestUtils::AllocationCounter counter;
            counter.begin();

            if (iteration % 3 == 1)
                processor->processBlockBypassed (buffer, midi);
            else
                processor->processBlock (buffer, midi);

            counter.end();
            allocations += counter.getCount();
        }

        expect (allocations == 0,
                "Bypass paths should be allocation-free; found " + juce::String (static_cast<int> (allocations))
                    + " allocations");
    }
};
