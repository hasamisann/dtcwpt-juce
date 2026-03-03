#include "GateAudioProcessor.h"
#include "gui/GateEditor.h"
#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
// Plugin entry point — required by JUCE
//==============================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GateAudioProcessor();
}

juce::AudioProcessorEditor* GateAudioProcessor::createEditor()
{
    return new GateEditor (*this);
}

bool GateAudioProcessor::hasEditor() const
{
    return true;
}
