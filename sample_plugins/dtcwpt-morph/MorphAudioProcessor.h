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
 *  - Topology changes: the engine for a new topology is built on a non-audio thread
 *    and handed to the audio thread through EngineHandoff (see below).
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
#include "SpectrumDataBridge.h"
#include "../common/AnalyzerLatencyAligner.h"
#include "../common/EngineHandoff.h"

// Standard library
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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
class MorphAudioProcessor : public juce::AudioProcessor,
                            private juce::Timer
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

    /** Destructor. Destroys every engine, including the ones retired by the audio thread. */
    ~MorphAudioProcessor() override;

    //==============================================================================
    // AudioProcessor interface — lifecycle
    //==============================================================================

    /**
     * @brief Allocates DSP resources and prepares the DTCWPTProcessor.
     *
     * Builds the engine for the committed topology (DTCWPTProcessor + MorphBandProcessor +
     * analyzer aligner), installs it directly (the audio thread is not running), allocates
     * double-precision conversion buffers, and reports the latency to the host.
     *
     * @param sampleRate     Host sample rate in Hz.
     * @param samplesPerBlock Maximum block size in samples.
     */
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;

    /**
     * @brief Releases all DSP resources.
     *
     * Destroys the engines and clears the raw MorphBandProcessor observer
     * pointer. Double-precision conversion buffers are also cleared.
     */
    void releaseResources() override;

    /**
     * @brief Processes one block of audio.
     *
     * Per-block operations (in order):
     *  1. Take a pending engine from EngineHandoff, if any. A new engine is cross-faded in
     *     over ~10 ms (old engine output fades out, new engine input fades in; both run
     *     during the fade) and the old engine is handed back for destruction off the
     *     audio thread.
     *  2. Read APVTS atomic floats; call setTargets() on the running engine(s).
     *  3. Convert float main buffer → double mainDoubleBuffer_.
     *  4. Build sidechain double buffer (bus 1 if enabled, else copy of main).
     *  5. dtcwptMain_->processSidechain(scDoubleBuffer_).
     *  6. dtcwptMain_->processBlock(mainDoubleBuffer_).
     *  7. Convert double output → float main buffer.
     *  8. Push mono-averaged input + output to spectrumBridge_.
     *
     * Real-time safe: no allocations, no locks, no ValueTree / std::string access, no host
     * notifications. Engine construction and destruction happen on other threads.
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
     * Returns a new MorphEditor instance backed by this processor.
     */
    juce::AudioProcessorEditor* createEditor() override;

    /** Returns true — MorphEditor is now wired up. */
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
     * Writes a copy of the APVTS state with the committed topology attached as the
     * "topology" property. The live ValueTree is never written to by the audio thread.
     *
     * @param destData  Output memory block.
     */
    void getStateInformation (juce::MemoryBlock& destData) override;

    /**
     * @brief Restores parameters + topology from a binary block.
     *
     * Uses APVTS::replaceState(). The "topology" property is then validated; a valid one
     * is committed (the engine is built here and handed to the audio thread), an invalid
     * one is reported and the committed topology is kept (or the default is used if no
     * topology was ever committed).
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
     *  - threshold:  NormalisableRange<float>(-200, 0),  default -20.0 [dB]
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
     * @brief Requests a topology change. Safe to call from any non-audio thread.
     *
     * Validates the topology by building the engine on the calling thread, updates the
     * latency reported to the host, makes the topology the committed (saved) one and
     * hands the engine to the audio thread. Later requests supersede earlier ones that
     * the audio thread has not picked up yet.
     *
     * @return true if the topology was accepted. A rejected topology (invalid, or deeper
     *         than the plugin supports) emits the shared invalid-topology diagnostic and
     *         leaves the committed topology, engine and latency unchanged.
     */
    bool requestTopologyChange (const std::vector<std::string>& destinations);

    /** Returns the committed topology (the one that is saved and that the engine runs). Non-audio threads only. */
    std::vector<std::string> getCommittedTopology() const;

    /**
     * @brief Counter that advances every time the committed topology changes.
     *
     * Lock-free; the GUI polls it and calls getCommittedTopology() when it changed.
     */
    std::uint64_t getTopologyGeneration() const noexcept { return topologyGeneration_.load (std::memory_order_acquire); }

    /**
     * @brief Returns a reference to the APVTS for parameter access.
     *
     * Exposed publicly to allow test code and GUI to query parameter values
     * and manage state without accessing private members directly.
     *
     * @return Reference to AudioProcessorValueTreeState.
     */
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts_; }

    // Test seams. The *ForTest accessors that read audio-thread state must be called from the
    // thread that calls processBlock (or while no audio is running).

    /** requestTopologyChange() followed by taking the new engine as the audio thread would, without a fade. */
    bool applyTopologyCandidateForTest (const std::vector<std::string>& candidateDestinations);
    /** Takes a pending engine the way processBlock does, without a fade. Returns true if one was taken. */
    bool adoptPendingEngineForTest() noexcept;
    int getCommittedTopologyDepthForTest() const;
    const MorphBandProcessor* getObservedMorphProcessorForTest() const noexcept { return active_ != nullptr ? active_->morph : nullptr; }
    const dtcwpt::DTCWPTProcessor* getEngineIdentityForTest() const noexcept { return active_ != nullptr ? active_->engine.get() : nullptr; }
    int getAnalyzerInputDelaySamplesForTest() const noexcept;
    bool consumeSpectrumSnapshotForTest (std::array<float, SpectrumDataBridge::kSlotSize>& inputOut,
                                         std::array<float, SpectrumDataBridge::kSlotSize>& outputOut);

private:
    //==============================================================================
    // Engine bundle (built off the audio thread, owned by the audio thread while in use)
    //==============================================================================

    /** Everything needed to run one topology. Built by buildEngineBundle(), destroyed off the audio thread. */
    struct EngineBundle
    {
        /** Main DT-CWPT analysis/synthesis engine. Owns the MorphBandProcessor. */
        std::unique_ptr<dtcwpt::DTCWPTProcessor> engine;

        /** Raw observer of the MorphBandProcessor owned by `engine` (used for setTargets() each block). */
        MorphBandProcessor* morph { nullptr };

        /** Delays the analyzer input by the engine latency. */
        AnalyzerLatencyAligner aligner;

        /** Engine latency in samples. */
        int latency { 0 };
    };

    //==============================================================================
    // Control side (message / host threads) — everything below is guarded by controlMutex_
    //==============================================================================

    /** Builds the engine for `destinations` with the cached sample rate and block size. Throws std::invalid_argument. */
    std::unique_ptr<EngineBundle> buildEngineBundle (const std::vector<std::string>& destinations) const;

    /** Validates, builds and publishes `candidate`; see requestTopologyChange(). */
    bool commitTopologyLocked (const std::vector<std::string>& candidate);

    /** Updates the committed topology and advances the generation counter. */
    void setCommittedTopologyLocked (const std::vector<std::string>& destinations);

    /** Reports the new latency and hands the bundle to the audio thread (deferring it if the retire ring is full). */
    void publishBundleLocked (std::unique_ptr<EngineBundle> bundle);

    /** prepareToPlay(): builds the committed topology and installs it directly. */
    void installEngineForPrepareLocked();

    /** Retires the engines the audio thread holds. Only valid while the audio thread is not running. */
    void releaseEnginesLocked();

    /** Destroys engines retired by the audio thread and retries a deferred publish. */
    void reclaimLocked();

    /** juce::Timer: periodic reclaim on the message thread. */
    void timerCallback() override;

    //==============================================================================
    // Audio side
    //==============================================================================

    /**
     * @brief Takes the pending engine, if any, and starts switching to it.
     *
     * Does nothing while a previous switch is still fading. With `immediate`, or when the running
     * engine has not processed any audio yet, the switch is instant instead of a cross-fade.
     *
     * @return true if a new engine was taken.
     */
    bool acquirePendingEngine (bool immediate) noexcept;

    /** Gain of the incoming engine's input at fade position `position` (the outgoing output uses 1 - gain). */
    double fadeInGain (int position) const noexcept;

    //==============================================================================
    // APVTS — owns all automatable parameters
    //==============================================================================

    /** AudioProcessorValueTreeState — manages all automatable parameters. */
    juce::AudioProcessorValueTreeState apvts_;

    //==============================================================================
    // Thread-bridge objects (owned here, shared by reference with GUI)
    //==============================================================================

    /** Hand-off of built engines to the audio thread and of retired engines back. */
    EngineHandoff<EngineBundle> handoff_;

    /** Lock-free ring buffer passing spectrum samples to the GUI thread. */
    SpectrumDataBridge spectrumBridge_;

    //==============================================================================
    // Audio-thread state (written by the audio thread; by other threads only while audio is stopped)
    //==============================================================================

    /** Engine that produces the output (the incoming engine while a switch is fading). */
    EngineBundle* active_ { nullptr };

    /** Outgoing engine while a switch is fading, otherwise nullptr. */
    EngineBundle* fadingOut_ { nullptr };

    /** Samples of the current fade already rendered. */
    int fadePosition_ { 0 };

    /** Length of the switch cross-fade (~10 ms), set in prepareToPlay(). */
    int fadeLength_ { 1 };

    /** False until `active_` has processed a block; an engine with no history is replaced without a fade. */
    bool activeHasProcessed_ { false };

    /** Main input/output buffer at double precision (for DTCWPTProcessor). */
    juce::AudioBuffer<double> mainDoubleBuffer_;

    /** Sidechain input buffer at double precision. */
    juce::AudioBuffer<double> scDoubleBuffer_;

    /** Output of the outgoing engine during a switch. */
    juce::AudioBuffer<double> fadeOutBuffer_;

    /** Pre-allocated mono scratch buffer for input spectrum snapshot (single channel). */
    juce::AudioBuffer<float> inputMonoScratch_;

    /** Pre-allocated mono scratch buffer for output spectrum snapshot (single channel). */
    juce::AudioBuffer<float> outputMonoScratch_;

    /** Pre-allocated mono scratch buffer for latency-aligned analyzer input. */
    juce::AudioBuffer<float> alignedInputMonoScratch_;

    //==============================================================================
    // Control-side state
    //==============================================================================

    /** Serialises every non-audio entry point (requests, prepare/release, state save/restore, reclaim). Never taken on the audio thread. */
    mutable std::mutex controlMutex_;
    int committedLatency_ { 0 }; // guarded by controlMutex_; notify the host after unlocking

    /** Committed topology: what is saved in the state and what the engine runs (authoritative copy). */
    std::vector<std::string> committedDestinations_;

    /** Last topology that was built successfully; fallback if a not-yet-validated topology is rejected at prepare. */
    std::vector<std::string> lastBuiltDestinations_;

    /** Tracks whether a valid topology has already been built. */
    bool hasCommittedValidTopology_ { false };

    /** True between prepareToPlay() and releaseResources(); engines can only be built while prepared. */
    bool prepared_ { false };

    /** Cached sample rate (from prepareToPlay). */
    double currentSampleRate_ { 44100.0 };

    /** Cached maximum block size (from prepareToPlay). */
    int currentBlockSize_ { 512 };

    /** Bundle that could not be published because too many engines were still alive; retried by reclaimLocked(). */
    std::unique_ptr<EngineBundle> deferredBundle_;

    /** Advances whenever committedDestinations_ changes. */
    std::atomic<std::uint64_t> topologyGeneration_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphAudioProcessor)
};
