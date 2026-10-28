#pragma once

/**
 * @file GateAudioProcessor.h
 * @brief DT-CWPT Gate Plugin AudioProcessor — full implementation header.
 *
 * GateAudioProcessor is the JUCE AudioProcessor for the DT-CWPT Gate plugin.
 * It manages:
 *  - 31 automatable threshold parameters (ISO 1/3-octave points) via APVTS; see GateThresholdCurve.h.
 *  - A DTCWPTProcessor for real-time wavelet analysis/synthesis.
 *  - A GateBandProcessor for subband hard gating.
 *  - Thread-safe topology change handoff via TopologyState.
 *  - Audio → GUI spectrum data via SpectrumDataBridge.
 *  - Audio → GUI band RMS levels via BandLevelBridge.
 *  - Float ↔ Double conversion at the plugin I/O boundary.
 *  - State serialisation via APVTS.
 */

// JUCE
#include <juce_audio_processors/juce_audio_processors.h>

// dtcwpt module
#include <dtcwpt/dtcwpt_processor.h>

// Plugin classes
#include "GateBandProcessor.h"
#include "GateThresholdCurve.h"
#include "TopologyState.h"
#include "SpectrumDataBridge.h"
#include "BandLevelBridge.h"
#include "../common/AnalyzerLatencyAligner.h"

// Standard library
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <atomic>

//==============================================================================
// GateAudioProcessor
//==============================================================================

/**
 * @brief Main JUCE AudioProcessor for the DT-CWPT Gate plugin.
 *
 * Provides stereo main I/O. Processes audio at double precision internally;
 * converts at the plugin boundary.
 * Formats: VST3 and Standalone.
 * Plugin ID: "Dtcg", company: "LTSU".
 */
class GateAudioProcessor : public juce::AudioProcessor
{
public:
    //==============================================================================
    // Constructor / Destructor
    //==============================================================================

    /**
     * @brief Constructs the processor.
     *
     * Initialises the APVTS with the 31 threshold point parameters and sets the default topology
     * (6-level DWT: H, LH, LLH, LLLH, LLLLH, LLLLLH, LLLLLL) as a ValueTree property.
     */
    GateAudioProcessor();

    /** Destructor. */
    ~GateAudioProcessor() override = default;

    //==============================================================================
    // AudioProcessor interface — lifecycle
    //==============================================================================

    /**
     * @brief Allocates DSP resources and prepares the DTCWPTProcessor.
     */
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;

    /**
     * @brief Releases all DSP resources.
     */
    void releaseResources() override;

    /**
     * @brief Processes one block of audio.
     */
    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midiMessages) override;

    //==============================================================================
    // AudioProcessor interface — bus layout
    //==============================================================================

    /**
     * @brief Returns true if the given bus layout is supported.
     *
     * Accepts: stereo main input + stereo main output only.
     */
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    //==============================================================================
    // AudioProcessor interface — editor
    //==============================================================================

    /**
     * @brief Creates the plugin editor.
     */
    juce::AudioProcessorEditor* createEditor() override;

    /** Returns true — Editor is wired up. */
    bool hasEditor() const override;

    //==============================================================================
    // AudioProcessor interface — plugin identity
    //==============================================================================

    /** Returns "DT-CWPT Gate". */
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
     */
    void getStateInformation (juce::MemoryBlock& destData) override;

    /**
     * @brief Restores parameters + topology from a binary block.
     */
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Parameter layout factory
    //==============================================================================

    /**
     * @brief Creates the APVTS parameter layout: 31 threshold points (ISO 1/3 octave, 20 Hz–20 kHz)
     *        and the lowest-band bypass.
     */
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** @brief Parameter ID of threshold point @p pointIndex (0..30), e.g. "threshold_1khz". */
    static juce::String getThresholdParameterId (int pointIndex);

    //==============================================================================
    // Data bridge accessors
    //==============================================================================

    /** @brief Returns a reference to the lock-free audio→GUI spectrum bridge. */
    SpectrumDataBridge& getSpectrumBridge() noexcept { return spectrumBridge_; }

    /** @brief Returns a reference to the atomic topology change handoff state. */
    TopologyState& getTopologyState() noexcept { return topoState_; }

    /** @brief Returns a reference to the band level bridge. */
    BandLevelBridge& getBandLevelBridge() noexcept { return bandLevelBridge_; }

    /** @brief Returns a reference to the APVTS for parameter access. */
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts_; }

    /** @brief Resolves topology destinations persisted in APVTS state. */
    std::vector<std::string> getStoredTopologyDestinations() const;

    bool applyTopologyCandidateForTest (const std::vector<std::string>& candidateDestinations) noexcept;
    const std::vector<std::string>& getCommittedDestinationsForTest() const noexcept { return currentDestinations_; }
    juce::String getPersistedTopologyStringForTest() const;
    int getAnalyzerInputDelaySamplesForTest() const noexcept;
    double getLastThresholdLinearForTest (int bandIndex) const noexcept;
    bool getLastBypassLowestForTest() const noexcept;
    const GateBandProcessor* getObservedGateProcessorForTest() const noexcept { return gateProcessor_; }
    const dtcwpt::DTCWPTProcessor* getEngineIdentityForTest() const noexcept { return dtcwptMain_.get(); }

private:
    //==============================================================================
    // Private helpers
    //==============================================================================

    /**
     * @brief (Re)creates the DTCWPTProcessor using currentDestinations_.
     */
    void rebuildDTCWPT();
    bool tryApplyTopologyCandidate (const std::vector<std::string>& candidateDestinations) noexcept;

    //==============================================================================
    // APVTS — owns all automatable parameters
    //==============================================================================

    juce::AudioProcessorValueTreeState apvts_;
    
    /** Cached raw pointers to the 31 threshold point parameters. */
    std::array<std::atomic<float>*, gate_threshold::kNumPoints> thresholdRaw_ {};
    std::array<double, GateBandProcessor::kMaxBands> lastThresholdsForTest_ {};

    /** Cached raw pointer to the bypass lowest band parameter. */
    std::atomic<float>* bypassLowestRaw_ { nullptr };
    bool lastBypassLowestForTest_ { false };

    //==============================================================================
    // DSP objects
    //==============================================================================

    std::unique_ptr<dtcwpt::DTCWPTProcessor> dtcwptMain_;
    GateBandProcessor* gateProcessor_ { nullptr };

    //==============================================================================
    // Thread-bridge objects
    //==============================================================================

    TopologyState topoState_;
    SpectrumDataBridge spectrumBridge_;
    BandLevelBridge bandLevelBridge_;

    //==============================================================================
    // Double-precision conversion buffers
    //==============================================================================

    juce::AudioBuffer<double> mainDoubleBuffer_;

    //==============================================================================
    // Mono scratch buffers for spectrum bridge
    //==============================================================================

    juce::AudioBuffer<float> inputMonoScratch_;
    juce::AudioBuffer<float> outputMonoScratch_;
    juce::AudioBuffer<float> alignedInputMonoScratch_;

    AnalyzerLatencyAligner analyzerInputAligner_;
    int analyzerInputAlignerMaxDelay_ { 0 };

    //==============================================================================
    // Cached prepare() state
    //==============================================================================

    std::vector<std::string> currentDestinations_;
    double currentSampleRate_ { 44100.0 };
    int currentBlockSize_ { 512 };
    bool hasCommittedValidTopology_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GateAudioProcessor)
};
