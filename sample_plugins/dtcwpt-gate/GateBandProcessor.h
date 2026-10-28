#pragma once

#include "dtcwpt/dtcwpt_band_processor.h"
#include <array>
#include <limits>
#include <juce_core/juce_core.h>

/**
 * @brief Subband noise gate processor for DT-CWPT.
 *
 * Gates the complex subband signals with a per-band, per-channel dynamics model:
 *  - envelope follower on |c| (attack when rising, release when falling),
 *  - open/close decision with hysteresis and hold,
 *  - gain smoothed by attack/release towards 1 (open) or the range gain (closed),
 *  - optional stereo link (gate decision on the larger L/R envelope),
 *  - threshold changes smoothed by a linear ramp on the subband time axis.
 *
 * All time constants are given in milliseconds and converted with the subband sample rate
 * fs_b = fs / 2^level (level = depth of the destination node), because a band at depth
 * `level` is decimated by 2^level.
 * Also computes pre-gate RMS levels for GUI display.
 */
class GateBandProcessor : public dtcwpt::BandProcessor
{
public:
    static constexpr int kMaxBands = 256;
    static constexpr int kMaxChannels = 2;
    /** Duration of the linear ramp applied when a threshold changes (in time, independent of the band's rate). */
    static constexpr double kThresholdSmoothingMs = 20.0;

    GateBandProcessor() noexcept;
    ~GateBandProcessor() override = default;

    /** Prepares the processor to process audio. Initializes internal arrays. */
    void prepare(double sampleRate, int maxSamplesPerBand, int numBands, int numChannels) override;

    /** Processes all subbands. Applies the gate and computes the RMS levels. */
    void processAllBands(dtcwpt::BandData& data) noexcept override;

    /** Resets the internal state (silences the band levels). */
    void reset() override;

    /**
     * @brief Set per-band thresholds (linear amplitude). Called per-block from AudioProcessor.
     * @param thresholds Pointer to an array of numBands thresholds.
     * @param numBands The number of valid elements in the thresholds array.
     */
    void setThresholds(const double* thresholds, int numBands) noexcept;

    /**
     * @brief Get pre-gate RMS levels (dB) for GUI display.
     * @param outLevelsDb Destination array (must be at least numBands_ large).
     * @return The number of bands written to outLevelsDb.
     */
    int getBandLevelsDb(float* outLevelsDb) const noexcept;

    /** Time-domain parameters shared by all bands. */
    struct DynamicsParams
    {
        double attackMs = 1.0;       ///< Envelope rise / gain opening time constant
        double holdMs = 50.0;        ///< Time the gate stays open after the level falls below the close threshold
        double releaseMs = 100.0;    ///< Envelope fall / gain closing time constant
        double hysteresisDb = 3.0;   ///< close threshold = threshold - hysteresis
        double rangeDb = -std::numeric_limits<double>::infinity(); ///< Closed-state gain; -inf mutes
        bool stereoLink = true;      ///< Decide on max(L, R) envelope and apply the same gain to both channels
    };

    /** Sets the dynamics parameters. Called per-block from AudioProcessor (audio thread). */
    void setDynamics(const DynamicsParams& params) noexcept;

    /** Enable or disable processing on the lowest-frequency band. */
    void setBypassLowest(bool bypass) noexcept { bypassLow_ = bypass; }

private:
    /** Subband is fundamental/DC if it falls on index matching (nodeId & (nodeId - 1)) == 0. */
    static bool isLowBand(int nodeId) noexcept { return nodeId > 0 && (nodeId & (nodeId - 1)) == 0; }

    struct ChannelState
    {
        double envelope = 0.0;  ///< Smoothed |c|
        double gain = 1.0;      ///< Smoothed gain applied to the coefficients
        int holdRemaining = 0;  ///< Remaining hold time in subband samples
        bool open = false;
        bool primed = false;    ///< False until the first sample initialises the gain to the closed gain
    };

    struct BandState
    {
        std::array<ChannelState, kMaxChannels> channels;
        double thresholdCurrent = 0.0;  ///< Threshold actually used (linear amplitude), ramps to thresholds_[b]
        double thresholdGoal = 0.0;     ///< Target of the ramp in progress
        double thresholdStep = 0.0;     ///< Per-sample increment of the ramp
        int rampRemaining = 0;
        bool thresholdInitialised = false;
    };

    std::array<double, kMaxBands> thresholds_;   // Linear amplitude
    std::array<BandState, kMaxBands> states_;
    DynamicsParams dynamics_;
    std::array<float, kMaxBands> bandLevelsDb_;  // Pre-gate RMS in dB
    int numBands_ = 0;
    double sampleRate_ = 44100.0;
    int maxSamplesPerBand_ = 0;
    int numChannels_ = 0;
    bool bypassLow_ = false;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GateBandProcessor)
};
