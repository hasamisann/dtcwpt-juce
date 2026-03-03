#pragma once

#include "dtcwpt/dtcwpt_band_processor.h"
#include <array>
#include <juce_core/juce_core.h>

/**
 * @brief Subband noise gate processor for DT-CWPT.
 *
 * Applies a hard gate to complex subband signals based on a per-band linear threshold.
 * Also computes pre-gate RMS levels for GUI display.
 */
class GateBandProcessor : public dtcwpt::BandProcessor
{
public:
    static constexpr int kMaxBands = 64;

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

private:
    std::array<double, kMaxBands> thresholds_;   // Linear amplitude
    std::array<float, kMaxBands> bandLevelsDb_;  // Pre-gate RMS in dB
    int numBands_ = 0;
    double sampleRate_ = 44100.0;
    int maxSamplesPerBand_ = 0;
    int numChannels_ = 0;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GateBandProcessor)
};
