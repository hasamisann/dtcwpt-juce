#pragma once

/**
 * @file MorphBandProcessor.h
 * @brief DT-CWPT Morph plugin subband processor.
 *
 * MorphBandProcessor is a concrete subclass of dtcwpt::BandProcessor that
 * implements per-subband spectral morphing between a main and sidechain input.
 *
 * The morph algorithm:
 * 1. Converts main and sidechain Re/Im subbands to magnitude/phase (polar form).
 * 2. Linearly interpolates (lerps) magnitudes: outMag = lerp(mainMag, scMag, magnitude).
 * 3. Applies a threshold gate to attenuate phase influence when the sidechain
 *    signal is below a minimum level.
 * 4. N-Lerps phases by representing them as unit vectors, lerping, then taking
 *    atan2 of the result (no renorm step required since atan2 is normalisation).
 * 5. Converts back to Re/Im.
 *
 * Parameters are smoothed with a 20 ms linear ramp defined on the host-sample
 * time axis (TimedLinearRamp). One processAllBands() call covers one engine
 * internal block of T host samples; subband sample s of a band at tree level L
 * sits at time tau = (s + 0.5) * 2^L inside that block, and every band and
 * channel uses the ramp value at that tau. Parameter changes are queued with
 * their position in the input stream by setTargets() and start their ramp at
 * the matching time inside the block.
 *
 * Thread safety: All public methods are called on the audio thread and are
 * real-time safe (noexcept, no allocations, no locks, no syscalls).
 */

// dtcwpt headers
#include <dtcwpt/dtcwpt_band_processor.h>
#include <dtcwpt/dtcwpt_complex_utils.h>

// Plugin headers
#include "TimedLinearRamp.h"

// JUCE headers
#include <juce_audio_basics/juce_audio_basics.h>

// Standard library
#include <array>
#include <cstdint>
#include <vector>

/**
 * @brief Spectral morph processor operating on DT-CWPT subband data.
 *
 * Subclasses dtcwpt::BandProcessor. The dtcwpt::DTCWPTProcessor engine calls
 * processAllBands() after analysis, providing mutable pointers to all subband
 * Re/Im data for both the main and sidechain inputs.
 *
 * Usage:
 * @code
 *   auto proc = std::make_unique<MorphBandProcessor>();
 *   MorphBandProcessor* raw = proc.get();
 *   dtcwptProcessor->setBandProcessor(std::move(proc));
 *   // ...per block:
 *   raw->setTargets(mag, phase, threshLinear, bypassLow, bypassHigh, streamPosition);
 * @endcode
 */
class MorphBandProcessor : public dtcwpt::BandProcessor
{
public:
    //==============================================================================
    // Constructor / Destructor
    //==============================================================================

    MorphBandProcessor()  = default;
    ~MorphBandProcessor() = default;

    //==============================================================================
    // Parameter control (audio thread)
    //==============================================================================

    /** Maximum number of pending parameter events (see setTargets()). */
    static constexpr int kEventQueueCapacity = 64;

    /**
     * @brief Queues new parameter targets taking effect at a position in the input stream.
     *
     * Call once per host block, BEFORE the engine's processBlock() for that block,
     * with the number of host samples the plugin has fed to the engine so far
     * (the stream position of the first sample of this block). The engine
     * accumulates input into internal blocks of N samples; internal block m
     * covers stream positions [m*N, (m+1)*N) and its processAllBands() call
     * happens inside the host block that contains position (m+1)*N - 1. Every
     * event with a position below (m+1)*N has therefore been queued by then,
     * and processAllBands() starts the ramp toward the new targets at
     * (position - m*N) inside its block.
     *
     * Requests identical to the previous one are ignored. If the queue is full,
     * the two oldest events are merged into one that keeps the newer event's
     * position and values (i.e. the oldest one is dropped): the latest requests
     * always win and only the intermediate ramp starts are lost.
     * Positions must not decrease; a smaller position is treated as equal to the
     * previous one.
     *
     * @param magnitude      Target magnitude interpolation ratio [0.0, 1.0].
     *                       0 = use main magnitudes, 1 = use sidechain magnitudes.
     * @param phase          Target phase interpolation ratio [0.0, 1.0].
     *                       0 = use main phases, 1 = use sidechain phases.
     * @param threshLinear   Sidechain magnitude threshold (linear amplitude).
     *                       Phase influence is attenuated when scMag < threshold.
     * @param bypassLow      If true, pass the lowest-frequency subband (L-only path)
     *                       through unchanged.
     * @param bypassHigh     If true, pass the highest-frequency subband (H-only path)
     *                       through unchanged.
     * @param streamPosition Host samples fed to the engine before this block.
     */
    void setTargets (double magnitude, double phase,
                     double threshLinear, bool bypassLow, bool bypassHigh,
                     std::int64_t streamPosition) noexcept;

    //==============================================================================
    // BandProcessor overrides
    //==============================================================================

    /**
     * @brief Allocates scratch buffers and sets the 20 ms ramp length.
     *
     * Called by dtcwpt::DTCWPTProcessor::prepareToPlay(). Scratch buffers are
     * sized to maxSamplesPerBand. Also restarts the stream timeline (see reset()).
     *
     * @param sampleRate        Host sample rate (Hz)
     * @param maxSamplesPerBand Maximum number of subband samples per block
     * @param numBands          Number of destination bands
     * @param numChannels       Number of audio channels
     */
    void prepare (double sampleRate, int maxSamplesPerBand,
                  int numBands, int numChannels) override;

    /**
     * @brief Processes all subbands of one engine internal block.
     *
     * The block length T (host samples) is max over bands of
     * numSamples * 2^level, where level is the depth of the node ID
     * (floor(log2(id))); for a complete leaf set it is the same for every band.
     * For each band, channel and subband sample s:
     *   1. Takes the ramp values at tau = (s + 0.5) * 2^level (shared by all
     *      bands and channels; independent of bypass).
     *   2. Converts main and sidechain Re/Im to polar form.
     *   3. Lerps magnitudes.
     *   4. Applies threshold gate to attenuate phase influence.
     *   5. N-Lerps phases via unit-vector lerp + atan2.
     *   6. Converts back to Re/Im.
     * Afterwards the ramps and the stream timeline advance by T, also when no
     * sidechain is present (the samples are then left untouched).
     *
     * Called on the audio thread — real-time safe (noexcept, no allocations).
     *
     * @param data BandData containing all channels' subband data
     */
    void processAllBands (dtcwpt::BandData& data) override;

    /**
     * @brief Snaps all ramps to the latest requested targets, empties the event
     *        queue and restarts the stream timeline at position 0.
     *
     * Called when the engine (re)starts its input stream, so that stream
     * positions given to setTargets() count from the same origin.
     */
    void reset() override;

private:
    //==============================================================================
    // Bypass helpers
    //==============================================================================

    /**
     * @brief Returns true if the node ID represents the lowest-frequency band.
     *
     * The lowest-frequency (L-only) band is the leaf whose node ID is a power
     * of 2: (n & (n - 1)) == 0.
     *
     * @param nodeId Destination node ID from BandData::destinationIds
     * @return true if this is the low bypass band
     */
    static bool isLowBand  (int nodeId) noexcept;

    /**
     * @brief Returns true if the node ID represents the highest-frequency band.
     *
     * The highest-frequency (H-only) band is the leaf whose node ID has all
     * bits set: (n & (n + 1)) == 0.
     *
     * @param nodeId Destination node ID from BandData::destinationIds
     * @return true if this is the high bypass band
     */
    static bool isHighBand (int nodeId) noexcept;

    /**
     * @brief Returns the tree depth of a node ID (root = 1 -> 0, "H"/"L" -> 1, ...).
     *
     * Equal to floor(log2(nodeId)); a band at level L has 2^L times fewer
     * samples than the input stream. Returns 0 for non-positive IDs.
     */
    static int nodeLevel (int nodeId) noexcept;

    //==============================================================================
    // Time-axis parameter state
    //==============================================================================

    /** Parameter values requested at one point of the input stream. */
    struct Targets
    {
        double magnitude { 0.0 };
        double phase     { 0.0 };
        double threshold { 0.0 };
        bool   bypassLow  { true };
        bool   bypassHigh { false };

        bool operator== (const Targets& o) const noexcept
        {
            return TimedLinearRamp::sameValue (magnitude, o.magnitude)
                && TimedLinearRamp::sameValue (phase, o.phase)
                && TimedLinearRamp::sameValue (threshold, o.threshold)
                && bypassLow == o.bypassLow && bypassHigh == o.bypassHigh;
        }
    };

    struct TargetEvent
    {
        std::int64_t position { 0 };
        Targets      targets;
    };

    /** Parameter state valid from `tau` (host samples from the block start) on. */
    struct Segment
    {
        double          tau { 0.0 };
        TimedLinearRamp magnitude, phase, threshold;
        bool            bypassLow  { true };
        bool            bypassHigh { false };
    };

    /** Applies @p t to @p seg: starts ramps (bypass flags switch instantly). */
    void applyTargets (Segment& seg, const Targets& t) const noexcept;

    /** Builds segments_ for the internal block [blockStart_, blockStart_ + T) and dequeues the events it consumes. */
    void beginBlock (std::int64_t blockLength) noexcept;

    /** Advances the ramps and the timeline past the block. */
    void endBlock (std::int64_t blockLength) noexcept;

    /** Pending events, oldest first. Filled by setTargets(), drained by processAllBands(). */
    std::array<TargetEvent, kEventQueueCapacity> events_;
    int numEvents_ { 0 };

    /** Latest requested targets (the queue's last event, or the last consumed one). */
    Targets latestTargets_;

    /** Ramp state at the start of the next internal block. */
    Segment state_;

    /** Segments of the block being processed: one per consumed event plus the initial one. */
    std::array<Segment, kEventQueueCapacity + 1> segments_;
    int numSegments_ { 1 };

    /** Stream position of the first sample of the next internal block. */
    std::int64_t blockStart_ { 0 };

    /** Ramp length in host samples (sampleRate * 20 ms). */
    double rampSamples_ { 0.0 };

    //==============================================================================
    // Pre-allocated scratch buffers (allocated in prepare(); zero-alloc on audio thread)
    //==============================================================================

    std::vector<double> mainMag_;   ///< Scratch buffer for main subband magnitudes
    std::vector<double> mainPhase_; ///< Scratch buffer for main subband phases
    std::vector<double> scMag_;     ///< Scratch buffer for sidechain subband magnitudes
    std::vector<double> scPhase_;   ///< Scratch buffer for sidechain subband phases
    std::vector<double> magRatio_;  ///< Per-sample magnitude ratio of the current band
    std::vector<double> phaseRatio_;///< Per-sample phase ratio of the current band
    std::vector<double> threshold_; ///< Per-sample threshold of the current band
    std::vector<char>   active_;    ///< Per-sample flag: 0 = bypassed

    //==============================================================================
    // Cached prepare() parameters
    //==============================================================================

    int    maxSamplesPerBand_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphBandProcessor)
};
