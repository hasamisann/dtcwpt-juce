/**
 * @file MorphAudioProcessorEditor.cpp
 * @brief Editor-related method implementations for MorphAudioProcessor.
 *
 * This translation unit is separated from MorphAudioProcessor.cpp to decouple
 * the processor from GUI dependencies, allowing the test target to link against
 * the DSP logic without requiring the full GUI source tree.
 */

#include "MorphAudioProcessor.h"
#include "gui/MorphEditor.h"

//==============================================================================
// Editor
//==============================================================================

juce::AudioProcessorEditor* MorphAudioProcessor::createEditor()
{
    return new MorphEditor (*this);
}

bool MorphAudioProcessor::hasEditor() const
{
    return true;
}
