#pragma once

/**
 * @file MorphAudioProcessor.h
 * @brief DT-CWPT Morph Plugin AudioProcessor entry point.
 *
 * MorphAudioProcessor is the main JUCE AudioProcessor for the DT-CWPT Morph plugin.
 * It manages plugin parameters via AudioProcessorValueTreeState (APVTS) and provides
 * stub implementations of all required AudioProcessor virtual methods.
 *
 * DSP logic, topology state, and GUI components will be added in subsequent tasks.
 */

// JUCE headers
#include <juce_audio_processors/juce_audio_processors.h>

/**
 * @brief Main JUCE AudioProcessor for the DT-CWPT Morph plugin.
 *
 * Provides stereo main I/O with an optional stereo sidechain auxiliary bus.
 * Formats: VST3 and Standalone.
 * Plugin ID: "DtcwptMorph", company: "hasamisann".
 *
 * All DSP implementation, parameter connections, and GUI creation are deferred
 * to later tasks (impl-005 through impl-010).
 */
class MorphAudioProcessor : public juce::AudioProcessor
{
public:
    //==============================================================================
    // Constructor / Destructor
    //==============================================================================

    /** Constructs the processor and initialises the APVTS with an empty parameter layout. */
    MorphAudioProcessor();

    /** Destructor. */
    ~MorphAudioProcessor() override = default;

    //==============================================================================
    // AudioProcessor interface — lifecycle
    //==============================================================================

    /** Called by host before playback starts. Reserve resources here. */
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;

    /** Called by host when playback stops. Release resources here. */
    void releaseResources() override;

    /** Processes audio. Stub implementation — passes audio through unchanged. */
    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midiMessages) override;

    //==============================================================================
    // AudioProcessor interface — bus layout
    //==============================================================================

    /**
     * @brief Returns true if the given bus layout is supported.
     *
     * Accepts: stereo main input + stereo main output + optional stereo sidechain input.
     */
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    //==============================================================================
    // AudioProcessor interface — editor
    //==============================================================================

    /** Returns nullptr — GUI is implemented in task 010. */
    juce::AudioProcessorEditor* createEditor() override;

    /** Returns false — no editor is provided yet. */
    bool hasEditor() const override;

    //==============================================================================
    // AudioProcessor interface — plugin identity
    //==============================================================================

    /** Returns the plugin name. */
    const juce::String getName() const override;

    bool  acceptsMidi()  const override;
    bool  producesMidi() const override;
    bool  isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    // AudioProcessor interface — programs
    //==============================================================================

    int  getNumPrograms()    override;
    int  getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    // AudioProcessor interface — state serialisation
    //==============================================================================

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Parameter layout factory
    //==============================================================================

    /**
     * @brief Creates the APVTS parameter layout.
     *
     * Returns an empty layout in this stub implementation.
     * Parameters will be added in task 005.
     *
     * @return Empty ParameterLayout
     */
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    //==============================================================================
    // Members
    //==============================================================================

    /** AudioProcessorValueTreeState — manages all automatable parameters. */
    juce::AudioProcessorValueTreeState apvts_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphAudioProcessor)
};
