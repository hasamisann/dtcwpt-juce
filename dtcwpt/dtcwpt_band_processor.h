#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace dtcwpt {

/**
 * @brief Per-band data view for a single channel.
 *
 * Holds mutable pointers to Re/Im subband data along with the sample count.
 */
struct ChannelBandView {
    double* re;             ///< Pointer to real part of subband data (mutable)
    double* im;             ///< Pointer to imaginary part of subband data (mutable)
    size_t numSamples;      ///< Number of subband samples in this band
};

/**
 * @brief All bands for all channels — passed to processAllBands().
 *
 * Contains main input bands and optionally sidechain bands, along with metadata.
 * Uses raw pointers into the processor's existing buffers for zero-copy access.
 */
struct BandData {
    /// Main input: bands[channelIndex][bandIndex]
    /// bandIndex is in destination order (same as TopologyConfig::destinations)
    std::vector<std::vector<ChannelBandView>> bands;

    /// Sidechain input: sidechainBands[channelIndex][bandIndex]
    /// Empty vector if no sidechain is active.
    std::vector<std::vector<ChannelBandView>> sidechainBands;

    int numChannels;        ///< Number of audio channels
    int numBands;           ///< Number of destination bands
    bool hasSidechain;      ///< True if sidechain data is available

    /// Destination node IDs in band order (for topology-aware processing)
    const std::vector<int>* destinationIds;
};

/**
 * @brief Abstract interface for user-defined subband processing.
 *
 * Users implement this interface to process subband data between analysis
 * and synthesis stages of the DT-CWPT pipeline.
 *
 * Thread safety: All methods are called on the audio thread and must be
 * real-time safe (no allocations, no locks, no syscalls).
 */
class BandProcessor {
public:
    virtual ~BandProcessor() = default;

    /**
     * @brief Called when DTCWPTProcessor::prepareToPlay() runs or when
     *        setBandProcessor() is called after prepare.
     *
     * @param sampleRate        Host sample rate (Hz)
     * @param maxSamplesPerBand Maximum subband samples per block (per band)
     * @param numBands          Number of destination bands
     * @param numChannels       Number of audio channels
     */
    virtual void prepare(double sampleRate, int maxSamplesPerBand,
                         int numBands, int numChannels) = 0;

    /**
     * @brief Process all bands at once.
     *
     * Default implementation iterates processBand() for each channel and band.
     * Override this for cross-band or cross-channel algorithms.
     *
     * Called on the audio thread — must be real-time safe.
     *
     * @param data BandData containing all channels' and bands' data
     */
    virtual void processAllBands(BandData& data);

    /**
     * @brief Process a single band.
     *
     * Called by the default processAllBands() implementation.
     * Override this for simple per-band processing.
     *
     * Called on the audio thread — must be real-time safe.
     *
     * @param bandIndex  Index into the destination array (0-based)
     * @param re         Mutable pointer to Re subband samples
     * @param im         Mutable pointer to Im subband samples
     * @param numSamples Number of subband samples
     */
    virtual void processBand(int bandIndex, double* re, double* im,
                             size_t numSamples);

    /**
     * @brief Reset internal state (e.g., when playback restarts).
     */
    virtual void reset() = 0;
};

/**
 * @brief Function type for per-band processing callback.
 *
 * Used by makeLambdaProcessor() to create a BandProcessor from a lambda.
 *
 * @param bandIndex  Index into the destination array (0-based)
 * @param re         Mutable pointer to Re subband samples
 * @param im         Mutable pointer to Im subband samples
 * @param numSamples Number of subband samples
 */
using BandProcessorFunc = std::function<void(int bandIndex, double* re, double* im, size_t numSamples)>;

/**
 * @brief Create a BandProcessor from a lambda function.
 *
 * Convenience factory for simple per-band processing. The returned processor
 * delegates processBand() to the provided function. prepare() and reset() are no-ops.
 *
 * @param func Function to call for each band
 * @return std::unique_ptr<BandProcessor> owning the lambda processor
 */
std::unique_ptr<BandProcessor> makeLambdaProcessor(BandProcessorFunc func);

} // namespace dtcwpt
