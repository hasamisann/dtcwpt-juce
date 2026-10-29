#include "MorphAudioProcessor.h"
#include "GateAudioProcessor.h"
#include <dtcwpt/dtcwpt_processor.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <vector>

class IntegratedRegressionTests : public juce::UnitTest
{
public:
    IntegratedRegressionTests() : UnitTest("IntegratedRegressionTests") {}
    void runTest() override
    {
        beginTest("Streaming Morph ramp is invariant to 1 vs 64 sample callbacks");
        const auto one = morphRamp(1);
        const auto sixtyFour = morphRamp(64);
        double difference = 0.0;
        for (std::size_t i = 0; i < one.size(); ++i)
            difference = std::max(difference, std::abs(one[i] - sixtyFour[i]));
        expect(difference < 1.0e-10, "Maximum output difference: " + juce::String(difference, 12));

        checkBypassReset<MorphAudioProcessor>("Morph");
        checkBypassReset<GateAudioProcessor>("Gate");
    }
private:
    static std::vector<double> morphRamp(int blockSize)
    {
        dtcwpt::DTCWPTProcessor engine;
        auto morph = std::make_unique<MorphBandProcessor>();
        auto* observer = morph.get();
        engine.setBandProcessor(std::move(morph));
        dtcwpt::TopologyConfig topology;
        topology.destinations = { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" };
        engine.prepareToPlay(48000.0, 64, topology, 1);
        observer->setTargets(1.0, 0.0, 0.0, false, false, 0);
        std::vector<double> result;
        for (int position = 0; position < 4096; position += blockSize)
        {
            const int n = std::min(blockSize, 4096 - position);
            juce::AudioBuffer<double> main(1, n), sidechain(1, n);
            std::fill_n(main.getWritePointer(0), n, 1.0);
            std::fill_n(sidechain.getWritePointer(0), n, 2.0);
            engine.processBlock(main, &sidechain);
            result.insert(result.end(), main.getReadPointer(0), main.getReadPointer(0) + n);
        }
        return result;
    }
    template<class Processor> void checkBypassReset(const juce::String& name)
    {
        beginTest(name + ": reset clears the bypass delay history");
        Processor processor;
        processor.prepareToPlay(48000.0, 64);
        processor.getBypassParameter()->setValueNotifyingHost(1.0f);
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> block(2, 64);
        for (int i = 0; i < 32; ++i)
        {
            for (int ch = 0; ch < 2; ++ch) std::fill_n(block.getWritePointer(ch), 64, 0.5f);
            processor.processBlock(block, midi);
        }
        processor.reset();
        float peak = 0.0f;
        for (int i = 0; i < 32; ++i)
        {
            block.clear();
            processor.processBlock(block, midi);
            peak = std::max(peak, block.getMagnitude(0, 64));
        }
        expectEquals(peak, 0.0f);
    }
};
static IntegratedRegressionTests integratedRegressionTests;
