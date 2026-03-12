#include "../../sample_plugins/dtcwpt-gate/GateAudioProcessor.h"

juce::AudioProcessorEditor* GateAudioProcessor::createEditor()
{
    return nullptr;
}

bool GateAudioProcessor::hasEditor() const
{
    return false;
}
