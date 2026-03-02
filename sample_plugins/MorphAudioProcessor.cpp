/**
 * @file MorphAudioProcessor.cpp
 * @brief Implementation of the DT-CWPT Morph Plugin AudioProcessor stub.
 *
 * Provides stub implementations of all required AudioProcessor virtual methods.
 * The sidechain bus is added as a disabled-by-default auxiliary input.
 *
 * Full DSP implementation will be added in task 005.
 */

#include "MorphAudioProcessor.h"

//==============================================================================
// Constructor
//==============================================================================

MorphAudioProcessor::MorphAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts_ (*this, nullptr, "MorphParams", createParameterLayout())
{
}

//==============================================================================
// Lifecycle
//==============================================================================

void MorphAudioProcessor::prepareToPlay (double /*sampleRate*/, int /*samplesPerBlock*/)
{
    // Stub — DSP resources will be allocated in task 005.
}

void MorphAudioProcessor::releaseResources()
{
    // Stub — DSP resources will be released in task 005.
}

//==============================================================================
// Processing
//==============================================================================

void MorphAudioProcessor::processBlock (juce::AudioBuffer<float>& /*buffer*/,
                                        juce::MidiBuffer& /*midiMessages*/)
{
    // Stub — full morphing DSP will be implemented in task 005.
}

//==============================================================================
// Bus layout
//==============================================================================

bool MorphAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Require stereo main output
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // Require stereo main input
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // Sidechain input (bus index 1) must be either disabled or stereo
    const auto scLayout = layouts.getChannelSet (true /*isInput*/, 1 /*busIndex*/);
    if (! scLayout.isDisabled() && scLayout != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

//==============================================================================
// Editor
//==============================================================================

juce::AudioProcessorEditor* MorphAudioProcessor::createEditor()
{
    // GUI will be created in task 010.
    return nullptr;
}

bool MorphAudioProcessor::hasEditor() const
{
    return false;
}

//==============================================================================
// Plugin identity
//==============================================================================

const juce::String MorphAudioProcessor::getName() const
{
    return "DT-CWPT Morph";
}

bool MorphAudioProcessor::acceptsMidi()  const { return false; }
bool MorphAudioProcessor::producesMidi() const { return false; }
bool MorphAudioProcessor::isMidiEffect() const { return false; }

double MorphAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

//==============================================================================
// Programs
//==============================================================================

int MorphAudioProcessor::getNumPrograms()
{
    return 1;
}

int MorphAudioProcessor::getCurrentProgram()
{
    return 0;
}

void MorphAudioProcessor::setCurrentProgram (int /*index*/)
{
    // No programs — stub.
}

const juce::String MorphAudioProcessor::getProgramName (int /*index*/)
{
    return {};
}

void MorphAudioProcessor::changeProgramName (int /*index*/, const juce::String& /*newName*/)
{
    // No programs — stub.
}

//==============================================================================
// State serialisation
//==============================================================================

void MorphAudioProcessor::getStateInformation (juce::MemoryBlock& /*destData*/)
{
    // Stub — will be implemented in task 005.
}

void MorphAudioProcessor::setStateInformation (const void* /*data*/, int /*sizeInBytes*/)
{
    // Stub — will be implemented in task 005.
}

//==============================================================================
// Parameter layout factory
//==============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout MorphAudioProcessor::createParameterLayout()
{
    // Empty layout — parameters will be added in task 005.
    return {};
}

//==============================================================================
// Plugin entry point — required by JUCE
//==============================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MorphAudioProcessor();
}
