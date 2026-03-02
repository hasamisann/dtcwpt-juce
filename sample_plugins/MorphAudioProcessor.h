#pragma once

/**
 * @file MorphAudioProcessor.h
 * @brief DT-CWPT Morph Plugin AudioProcessor — full implementation header.
 *
 * MorphAudioProcessor is the JUCE AudioProcessor for the DT-CWPT Morph plugin.
 * It manages:
 *  - Five automatable parameters via APVTS (magnitude, phase, threshold,
 *    bypassLow, bypassHigh).
 *  - A DTCWPTProcessor for real-time wavelet analysis/synthesis.
 *  - A MorphBandProcessor for spectral morphing in the subband domain.
 *  - Thread-safe topology change handoff via TopologyState.
 *  - Audio → GUI spectrum data via SpectrumDataBridge.
 *  - Float ↔ Double conversion at the plugin I/O boundary.
 *  - State serialisation via APVTS (parameters + topology string property).
 */

// JUCE
#include <juce_audio_processors/juce_audio_processors.h>

// dtcwpt module
#include <dtcwpt/dtcwpt_processor.h>

// Plugin classes
#include "MorphBandProcessor.h"
#include "TopologyState.h"
#include "SpectrumDataBridge.h"

// Standard library
#include <memory>
#include <string>
#include <vector>

//==============================================================================
// Parameter ID strings
//==============================================================================

/**
 * @brief Compile-time parameter ID constants used by APVTS and processBlock.
 *
 * These strings are the authoritative identifiers for all five automatable
 * parameters. They are referenced in both createParameterLayout() and in
 * processBlock() when reading atomic float values from the APVTS.
 */
namespace ParamID
{
    inline constexpr const char* Magnitude  = "magnitude";
    inline constexpr const char* Phase      = "phase";
    inline constexpr const char* Threshold  = "threshold";
    inline constexpr const char* BypassLow  = "bypassLow";
    inline constexpr const char* BypassHigh = "bypassHigh";
} // namespace ParamID

//==============================================================================
// MorphAudioProcessor
//==============================================================================

/**
 * @brief Main JUCE AudioProcessor for the DT-CWPT Morph plugin.
 *
 * Provides stereo main I/O with an optional stereo sidechain auxiliary bus.
 * Processes audio at double precision internally; converts at the plugin boundary.
 * Formats: VST3 and Standalone.
 * Plugin ID: "DtcwptMorph", company: "hasamisann".
 */
class MorphAudioProcessor : public juce::AudioProcessor
{
public:
    //==============================================================================
    // Constructor / Destructor
    //==============================================================================

    /**
     * @brief Constructs the processor.
     *
     * Initialises the APVTS with all five parameters and sets the default topology
     * (6-level DWT: H, LH, LLH, LLLH, LLLLH, LLLLLH, LLLLLL) as a ValueTree property.
     */
    MorphAudioProcessor();

    /** Destructor. */
    ~MorphAudioProcessor() override = default;

    //==============================================================================
    // AudioProcessor interface — lifecycle
    //==============================================================================

    /**
     * @brief Allocates DSP resources and prepares the DTCWPTProcessor.
     *
     * Creates a DTCWPTProcessor with the current topology, creates a
     * MorphBandProcessor and transfers ownership to the engine (retaining a raw
     * observer pointer), allocates double-precision conversion buffers, and reports
     * the initial latency to the host.
     *
     * @param sampleRate     Host sample rate in Hz.
     * @param samplesPerBlock Maximum block size in samples.
     */
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;

    /**
     * @brief Releases all DSP resources.
     *
     * Destroys the DTCWPTProcessor and clears the raw MorphBandProcessor observer
     * pointer. Double-precision conversion buffers are also cleared.
     */
    void releaseResources() override;

    /**
     * @brief Processes one block of audio.
     *
     * Per-block operations (in order):
     *  1. Topology dirty check — if set, rebuild DTCWPTProcessor and update latency.
     *  2. Read APVTS atomic floats; call morphProcessor_->setTargets().
     *  3. Convert float main buffer → double mainDoubleBuffer_.
     *  4. Build sidechain double buffer (bus 1 if enabled, else copy of main).
     *  5. dtcwptMain_->processSidechain(scDoubleBuffer_).
     *  6. dtcwptMain_->processBlock(mainDoubleBuffer_).
     *  7. Convert double output → float main buffer.
     *  8. Push mono-averaged input + output to spectrumBridge_.
     *
     * All code is real-time safe (noexcept, no allocations, no locks except
     * the brief topology-rebuild mutex hold).
     *
     * @param buffer        Audio I/O buffer (in: main + optional sidechain channels).
     * @param midiMessages  Unused.
     */
    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midiMessages) override;

    //==============================================================================
    // AudioProcessor interface — bus layout
    //==============================================================================

    /**
     * @brief Returns true if the given bus layout is supported.
     *
     * Accepts: stereo main input + stereo main output + optional stereo sidechain.
     * Rejects any mono layout.
     *
     * @param layouts  Proposed bus layout.
     * @return true if supported.
     */
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    //==============================================================================
    // AudioProcessor interface — editor
    //==============================================================================

    /**
     * @brief Creates the plugin editor.
     *
     * Returns nullptr — GUI (MorphEditor) will be wired in task 010.
     */
    juce::AudioProcessorEditor* createEditor() override;

    /** Returns false — editor not yet implemented. */
    bool hasEditor() const override;

    //==============================================================================
    // AudioProcessor interface — plugin identity
    //==============================================================================

    /** Returns "DT-CWPT Morph". */
    const juce::String getName() const override;

    bool   acceptsMidi()  const override;
    bool   producesMidi() const override;
    bool   isMidiEffect() const override;
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

    /**
     * @brief Serialises parameters + topology to a binary block.
     *
     * Uses APVTS::copyState() which includes all parameters and any ValueTree
     * properties (including the "topology" string property).
     *
     * @param destData  Output memory block.
     */
    void getStateInformation (juce::MemoryBlock& destData) override;

    /**
     * @brief Restores parameters + topology from a binary block.
     *
     * Uses APVTS::replaceState(). After restoring, reads the "topology" property
     * and signals topoState_ so the audio thread rebuilds the DTCWPTProcessor on
     * the next processBlock call.
     *
     * @param data        Pointer to serialised data.
     * @param sizeInBytes Size of serialised data in bytes.
     */
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Parameter layout factory
    //==============================================================================

    /**
     * @brief Creates the APVTS parameter layout with all five parameters.
     *
     * Parameters:
     *  - magnitude:  NormalisableRange<float>(0, 1),    default 0.0
     *  - phase:      NormalisableRange<float>(0, 1),    default 0.7
     *  - threshold:  NormalisableRange<float>(-60, 0),  default -20.0 [dB]
     *  - bypassLow:  AudioParameterBool,                default true
     *  - bypassHigh: AudioParameterBool,                default false
     *
     * @return Fully populated ParameterLayout.
     */
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //==============================================================================
    // Data bridge accessors (GUI components receive references at editor construction)
    //==============================================================================

    /**
     * @brief Returns a reference to the lock-free audio→GUI spectrum bridge.
     *
     * The bridge is owned by this processor and outlives any editor.
     *
     * @return Reference to SpectrumDataBridge.
     */
    SpectrumDataBridge& getSpectrumBridge() noexcept { return spectrumBridge_; }

    /**
     * @brief Returns a reference to the atomic topology change handoff state.
     *
     * The GUI's TopologyEditorView writes to this; the audio thread reads it.
     *
     * @return Reference to TopologyState.
     */
    TopologyState& getTopologyState() noexcept { return topoState_; }

    /**
     * @brief Returns a reference to the APVTS for parameter access.
     *
     * Exposed publicly to allow test code and GUI to query parameter values
     * and manage state without accessing private members directly.
     *
     * @return Reference to AudioProcessorValueTreeState.
     */
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts_; }

private:
    //==============================================================================
    // Private helpers
    //==============================================================================

    /**
     * @brief (Re)creates the DTCWPTProcessor using currentDestinations_.
     *
     * Called from prepareToPlay() and from processBlock() on topology change.
     * Not real-time safe (allocates) — must only be called when the topology dirty
     * flag is set and the mutex can be briefly held.
     */
    void rebuildDTCWPT();

    //==============================================================================
    // APVTS — owns all automatable parameters
    //==============================================================================

    /** AudioProcessorValueTreeState — manages all automatable parameters. */
    juce::AudioProcessorValueTreeState apvts_;

    //==============================================================================
    // DSP objects
    //==============================================================================

    /**
     * @brief Main DT-CWPT analysis/synthesis engine.
     *
     * Created/recreated in prepareToPlay() and on topology rebuild.
     * Owns the MorphBandProcessor via setBandProcessor(unique_ptr).
     */
    std::unique_ptr<dtcwpt::DTCWPTProcessor> dtcwptMain_;

    /**
     * @brief Raw observer pointer to the MorphBandProcessor.
     *
     * Ownership is transferred to dtcwptMain_ via setBandProcessor().
     * This raw pointer is retained for setTargets() calls each block.
     * Lifetime is tied to dtcwptMain_: reset to nullptr when dtcwptMain_ is destroyed.
     */
    MorphBandProcessor* morphProcessor_ { nullptr };

    //==============================================================================
    // Thread-bridge objects (owned here, shared by reference with GUI)
    //==============================================================================

    /** Atomic topology change handoff between GUI and audio threads. */
    TopologyState topoState_;

    /** Lock-free ring buffer passing spectrum samples to the GUI thread. */
    SpectrumDataBridge spectrumBridge_;

    //==============================================================================
    // Double-precision conversion buffers (pre-allocated in prepareToPlay)
    //==============================================================================

    /** Main input/output buffer at double precision (for DTCWPTProcessor). */
    juce::AudioBuffer<double> mainDoubleBuffer_;

    /** Sidechain input buffer at double precision. */
    juce::AudioBuffer<double> scDoubleBuffer_;

    //==============================================================================
    // Cached prepare() state
    //==============================================================================

    /** Current destination list (authoritative copy; also mirrored to apvts_.state). */
    std::vector<std::string> currentDestinations_;

    /** Current sample rate (cached from prepareToPlay). */
    double currentSampleRate_ { 44100.0 };

    /** Current maximum block size (cached from prepareToPlay). */
    int currentBlockSize_ { 512 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphAudioProcessor)
};
