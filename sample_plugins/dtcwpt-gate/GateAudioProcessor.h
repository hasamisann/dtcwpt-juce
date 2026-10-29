#pragma once

/**
 * @file GateAudioProcessor.h
 * @brief DT-CWPT Gate Plugin AudioProcessor — full implementation header.
 *
 * GateAudioProcessor is the JUCE AudioProcessor for the DT-CWPT Gate plugin.
 * It manages:
 *  - 256 automatable threshold parameters via APVTS.
 *  - A DTCWPTProcessor for real-time wavelet analysis/synthesis.
 *  - A GateBandProcessor for subband hard gating.
 *  - Topology changes: the engine for a new topology is built on a non-audio thread
 *    and handed to the audio thread through EngineHandoff.
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
#include "SpectrumDataBridge.h"
#include "BandLevelBridge.h"
#include "../common/AnalyzerLatencyAligner.h"
#include "../common/CommittedTailLength.h"
#include "../common/LatencyBypass.h"
#include "../common/DisplayConsumerCount.h"
#include "../common/EngineHandoff.h"

// Standard library
#include <cstdint>
#include <memory>
#include <mutex>
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
class GateAudioProcessor : public juce::AudioProcessor,
                          private juce::Timer
{
public:
    //==============================================================================
    // Constructor / Destructor
    //==============================================================================

    /**
     * @brief Constructs the processor.
     *
     * Initialises the APVTS with 256 threshold parameters. The committed topology starts as the
     * default (6-level DWT: H, LH, LLH, LLLH, LLLLH, LLLLLH, LLLLLL).
     */
    GateAudioProcessor();

    /** Destructor. Destroys every engine, including the ones retired by the audio thread. */
    ~GateAudioProcessor() override;

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
     *
     * Writes a copy of the APVTS state with the committed topology attached as the
     * "topology" property. The live ValueTree is never written to by the audio thread.
     */
    void getStateInformation (juce::MemoryBlock& destData) override;

    /**
     * @brief Restores parameters + topology from a binary block.
     *
     * A valid "topology" property is committed (the engine is built here and handed to the audio
     * thread). An invalid one is reported and the committed topology is kept (or the default is
     * used if no topology was ever committed).
     */
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Parameter layout factory
    //==============================================================================

    /**
     * @brief Creates the APVTS parameter layout with 256 threshold parameters.
     */
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //==============================================================================
    // Data bridge accessors
    //==============================================================================

    /** @brief Returns a reference to the lock-free audio→GUI spectrum bridge. */
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
     *         than Gate's depth limit) emits the shared invalid-topology diagnostic and
     *         leaves the committed topology, engine and latency unchanged.
     */
    bool requestTopologyChange (const std::vector<std::string>& destinations);

    /** @brief Returns the committed topology (the one that is saved and that the engine runs). Non-audio threads only. */
    std::vector<std::string> getCommittedTopology() const;

    /**
     * @brief Counter that advances every time the committed topology changes.
     *
     * Lock-free; the GUI polls it and calls getCommittedTopology() when it changed.
     */
    std::uint64_t getTopologyGeneration() const noexcept { return topologyGeneration_.load (std::memory_order_acquire); }

    /** @brief Returns a reference to the band level bridge. */
    BandLevelBridge& getBandLevelBridge() noexcept { return bandLevelBridge_; }

    /** @brief Returns a reference to the APVTS for parameter access. */
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts_; }

    // Test seams. The *ForTest accessors that read audio-thread state must be called from the
    // thread that calls processBlock (or while no audio is running).

    /** requestTopologyChange() followed by taking the new engine as the audio thread would, without a fade. */
    bool applyTopologyCandidateForTest (const std::vector<std::string>& candidateDestinations);
    /** Takes a pending engine the way processBlock does, without a fade. Returns true if one was taken. */
    bool adoptPendingEngineForTest() noexcept;
    int getAnalyzerInputDelaySamplesForTest() const noexcept;
    double getLastThresholdLinearForTest (int bandIndex) const noexcept;
    bool getLastBypassLowestForTest() const noexcept;
    const GateBandProcessor* getObservedGateProcessorForTest() const noexcept { return active_ != nullptr ? active_->gate : nullptr; }
    const dtcwpt::DTCWPTProcessor* getEngineIdentityForTest() const noexcept { return active_ != nullptr ? active_->engine.get() : nullptr; }

    void reset() override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorParameter* getBypassParameter() const override;
    DisplayConsumerCount& getDisplayConsumerCount() noexcept { return displayConsumers_; }
    bool isEngineAvailable() const noexcept { return engineAvailable_.load(std::memory_order_acquire); }
#if DTCWPT_ENABLE_TEST_SEAMS
    void failNextEngineBuildsForTest(dtcwpt::DTCWPTProcessor::PrepareCandidateFailpoint failpoint, int count) noexcept
    {
        engineBuildFailpointForTest_ = failpoint;
        engineBuildFailuresRemainingForTest_ = count;
    }
    std::vector<std::string> getCommittedDestinationsForTest() const { return getCommittedTopology(); }
    juce::String getPersistedTopologyStringForTest()
    {
        juce::MemoryBlock saved;
        getStateInformation(saved);
        return juce::ValueTree::readFromData(saved.getData(), saved.getSize()).getProperty("topology").toString();
    }
#endif
    static juce::String getThresholdParameterId(int pointIndex);
    GateBandProcessor::DynamicsParams getLastDynamicsForTest() const noexcept { return lastDynamicsForTest_; }

private:
    //==============================================================================
    // Engine bundle (built off the audio thread, owned by the audio thread while in use)
    //==============================================================================

    /** Everything needed to run one topology. Built by buildEngineBundle(), destroyed off the audio thread. */
    struct EngineBundle
    {
        /** Main DT-CWPT analysis/synthesis engine. Owns the GateBandProcessor. */
        std::unique_ptr<dtcwpt::DTCWPTProcessor> engine;

        /** Raw observer of the GateBandProcessor owned by `engine`. */
        GateBandProcessor* gate { nullptr };

        /** Delays the analyzer input by the engine latency. */
        AnalyzerLatencyAligner aligner;

        /** Engine latency in samples. */
        int latency { 0 };
        int tailSamples { 0 };
        std::int64_t inputPosition { 0 };
        bool fadeInOnStart { false };

        /** Number of bands (leaves) of the topology, clamped to GateBandProcessor::kMaxBands. */
        int numBands { 0 };
        std::vector<dtcwpt::NormalizedFrequencyRange> frequencyRanges;
    };

    //==============================================================================
    // Control side (message / host threads) — guarded by controlMutex_
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

    juce::AudioProcessorValueTreeState apvts_;

    std::array<std::atomic<float>*, gate_threshold::kNumPoints> thresholdRaw_ {};
    std::array<double, GateBandProcessor::kMaxBands> lastThresholdsForTest_ {};

    std::atomic<float>* bypassLowestRaw_ { nullptr };
    bool lastBypassLowestForTest_ { false };
    GateBandProcessor::DynamicsParams lastDynamicsForTest_ {};

    //==============================================================================
    // Thread-bridge objects
    //==============================================================================

    /** Hand-off of built engines to the audio thread and of retired engines back. */
    EngineHandoff<EngineBundle> handoff_;

    SpectrumDataBridge spectrumBridge_;
    BandLevelBridge bandLevelBridge_;

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

    juce::AudioBuffer<double> mainDoubleBuffer_;

    /** Output of the outgoing engine during a switch. */
    juce::AudioBuffer<double> fadeOutBuffer_;

    juce::AudioBuffer<float> inputMonoScratch_;
    juce::AudioBuffer<float> outputMonoScratch_;
    juce::AudioBuffer<float> alignedInputMonoScratch_;

    //==============================================================================
    // Control-side state
    //==============================================================================

    /** Serialises every non-audio entry point (requests, prepare/release, state save/restore, reclaim). Never taken on the audio thread. */
    void notifyCommittedState(int latency);
    void processBlockInternal(juce::AudioBuffer<float>&, bool forceBypass);
    void processChunk(juce::AudioBuffer<float>&, int start, int count);
    LatencyBypass latencyBypass_;
    std::atomic<float>* bypassRaw_ = nullptr;
    DisplayConsumerCount displayConsumers_;
    bool displayWasActive_ = false;
    CommittedTailLength committedTailLength_;
    std::atomic<bool> engineAvailable_ { false };
#if DTCWPT_ENABLE_TEST_SEAMS
    mutable dtcwpt::DTCWPTProcessor::PrepareCandidateFailpoint engineBuildFailpointForTest_
        = dtcwpt::DTCWPTProcessor::PrepareCandidateFailpoint::none;
    mutable int engineBuildFailuresRemainingForTest_ = 0;
#endif
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

    double currentSampleRate_ { 44100.0 };
    int currentBlockSize_ { 512 };

    /** Bundle that could not be published because too many engines were still alive; retried by reclaimLocked(). */
    std::unique_ptr<EngineBundle> deferredBundle_;

    /** Advances whenever committedDestinations_ changes. */
    std::atomic<std::uint64_t> topologyGeneration_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GateAudioProcessor)
};
