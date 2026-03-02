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
 * SmoothedValue targets are set once per host block via setTargets().
 * Per-sample advancement (getNextValue()) happens inside processAllBands() for
 * true per-sample smoothing — eliminating zipper noise at any block size.
 *
 * Thread safety: All public methods are called on the audio thread and are
 * real-time safe (noexcept, no allocations, no locks, no syscalls).
 */

// dtcwpt headers
#include <dtcwpt/dtcwpt_band_processor.h>
#include <dtcwpt/dtcwpt_complex_utils.h>

// JUCE headers
#include <juce_audio_basics/juce_audio_basics.h>

// Standard library
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
 *   raw->setTargets(mag, phase, threshLinear, bypassLow, bypassHigh);
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

    /**
     * @brief Sets SmoothedValue targets for the next block.
     *
     * Must be called once per host block, BEFORE processAllBands().
     * Per-sample advancement of the SmoothedValues happens inside processAllBands().
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
     */
    void setTargets (double magnitude, double phase,
                     double threshLinear, bool bypassLow, bool bypassHigh) noexcept;

    //==============================================================================
    // BandProcessor overrides
    //==============================================================================

    /**
     * @brief Allocates scratch buffers and initialises SmoothedValues.
     *
     * Called by dtcwpt::DTCWPTProcessor::prepareToPlay(). Scratch buffers are
     * sized to maxSamplesPerBand; SmoothedValues are set to 20 ms ramp time.
     *
     * @param sampleRate        Host sample rate (Hz)
     * @param maxSamplesPerBand Maximum number of subband samples per block
     * @param numBands          Number of destination bands
     * @param numChannels       Number of audio channels
     */
    void prepare (double sampleRate, int maxSamplesPerBand,
                  int numBands, int numChannels) override;

    /**
     * @brief Processes all subbands with the morph algorithm.
     *
     * For each non-bypassed band, for each channel, for each sample:
     *   1. Advances SmoothedValues by one step (per-sample smoothing).
     *   2. Converts main and sidechain Re/Im to polar form.
     *   3. Lerps magnitudes.
     *   4. Applies threshold gate to attenuate phase influence.
     *   5. N-Lerps phases via unit-vector lerp + atan2.
     *   6. Converts back to Re/Im.
     *
     * Called on the audio thread — real-time safe (noexcept, no allocations).
     *
     * @param data BandData containing all channels' subband data
     */
    void processAllBands (dtcwpt::BandData& data) override;

    /**
     * @brief Resets SmoothedValues to their current target values.
     *
     * Should be called when playback restarts to avoid ramp artefacts.
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

    //==============================================================================
    // Per-sample smoothed parameters
    //==============================================================================

    /** Magnitude interpolation ratio [0, 1]. Smoothed over 20 ms. */
    juce::SmoothedValue<double> magnitudeSmoothed_;

    /** Phase interpolation ratio [0, 1]. Smoothed over 20 ms. */
    juce::SmoothedValue<double> phaseSmoothed_;

    /** Sidechain threshold (linear amplitude). Smoothed over 20 ms. */
    juce::SmoothedValue<double> thresholdSmoothed_;

    //==============================================================================
    // Instantaneous bool parameters (no smoothing needed)
    //==============================================================================

    /** If true, the L-only (lowest-frequency) subband is passed through unchanged. */
    bool bypassLow_  { true };

    /** If true, the H-only (highest-frequency) subband is passed through unchanged. */
    bool bypassHigh_ { false };

    //==============================================================================
    // Pre-allocated scratch buffers (allocated in prepare(); zero-alloc on audio thread)
    //==============================================================================

    std::vector<double> mainMag_;   ///< Scratch buffer for main subband magnitudes
    std::vector<double> mainPhase_; ///< Scratch buffer for main subband phases
    std::vector<double> scMag_;     ///< Scratch buffer for sidechain subband magnitudes
    std::vector<double> scPhase_;   ///< Scratch buffer for sidechain subband phases

    //==============================================================================
    // Cached prepare() parameters
    //==============================================================================

    double sampleRate_        { 44100.0 };
    int    maxSamplesPerBand_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphBandProcessor)
};
