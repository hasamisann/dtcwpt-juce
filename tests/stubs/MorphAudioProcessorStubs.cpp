/**
 * @file MorphAudioProcessorStubs.cpp
 * @brief Stub implementations of GUI-dependent methods for test target.
 *
 * This file provides minimal stub implementations of createEditor() and
 * hasEditor() for the test target. The real implementations are in
 * MorphAudioProcessorEditor.cpp which is only compiled as part of the
 * plugin target to avoid pulling in GUI dependencies.
 */

#include "../sample_plugins/dtcwpt-morph/MorphAudioProcessor.h"

juce::AudioProcessorEditor* MorphAudioProcessor::createEditor()
{
    // Stub: return nullptr for headless tests
    // The real implementation returns new MorphEditor(*this)
    return nullptr;
}

bool MorphAudioProcessor::hasEditor() const
{
    // Stub: return false for headless tests
    // The real implementation returns true
    return false;
}
