#include "MorphAudioProcessor.h"
#include "GateAudioProcessor.h"
#include <juce_core/juce_core.h>

class TailPublicationTests : public juce::UnitTest
{
public:
    TailPublicationTests() : UnitTest("TailPublicationTests") {}
    void runTest() override
    {
        check<MorphAudioProcessor>("Morph");
        check<GateAudioProcessor>("Gate");
    }
private:
    struct Listener : juce::AudioProcessorListener
    {
        double notifiedTail = -1.0;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged(juce::AudioProcessor* processor, const ChangeDetails& details) override
        {
            if (details.latencyChanged || details.nonParameterStateChanged)
                notifiedTail = processor->getTailLengthSeconds();
        }
    };
    template<class Processor> void check(const juce::String& name)
    {
        beginTest(name + ": latency notification exposes the committed tail");
        Processor processor;
        Listener listener;
        processor.addListener(&listener);
        processor.prepareToPlay(48000.0, 512);
        expect(processor.getTailLengthSeconds() > 0.0);
        expectEquals(listener.notifiedTail, processor.getTailLengthSeconds());
        listener.notifiedTail = -1.0;
        processor.prepareToPlay(96000.0, 256);
        expectEquals(listener.notifiedTail, processor.getTailLengthSeconds());
        processor.removeListener(&listener);
    }
};
static TailPublicationTests tailPublicationTests;
