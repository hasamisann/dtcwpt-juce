#pragma once

#include "dtcwpt_analysis_node.h"
#include "dtcwpt_analysis_scheduler.h"
#include "dtcwpt_synthesis_node.h"
#include "dtcwpt_delay_buffer.h"
#include "dtcwpt_topology_limits.h"
#include "dtcwpt_topology_planner.h"
#include "dtcwpt_band_processor.h"
#include "dtcwpt_filter_structs.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dtcwpt {

/**
 * @brief Configuration structure for topology setup.
 */
struct TopologyConfig {
    std::vector<std::string> destinations;  ///< Destination path strings (e.g., "LLL", "LH")
    int maxDepth = topology_limits::kSupportedMaxDepth;  ///< Maximum tree depth (up to 12)
};

/**
 * @brief Node group structure.
 */
struct AnalysisNodeGroup {
    std::vector<AnalysisNode> nodes;  ///< Nodes in order corresponding to ids
    std::vector<int> ids;             ///< Node IDs corresponding to nodes
};

struct SynthesisNodeGroup {
    std::vector<SynthesisNode> nodes; ///< Nodes in order corresponding to ids
    std::vector<int> ids;             ///< Node IDs corresponding to nodes
};

/**
 * @brief Main DT-CWPT processing engine.
 *
 * Orchestrates the entire Dual-Tree Complex Wavelet Packet Transform pipeline.
 *
 * Pipeline order (deterministic for perfect reconstruction):
 * 1. Analysis (both Real and Imaginary trees)
 * 2. Delay Compensation (align subband phases before cross-band processing)
 * 3. BandProcessor (user-defined processing on aligned subbands)
 * 4. Synthesis (reconstruct from processed subbands)
 *
 * The delay compensation occurs BEFORE BandProcessor to ensure subbands are
 * temporally aligned when cross-band operations (morphing, etc.) are applied.
 * This ordering is critical for maintaining phase coherence in the reconstruction.
 */
class DTCWPTProcessor {
public:
    DTCWPTProcessor();
    ~DTCWPTProcessor() = default;

    // Non-copyable, non-movable (heavy resources)
    DTCWPTProcessor(const DTCWPTProcessor&) = delete;
    DTCWPTProcessor& operator=(const DTCWPTProcessor&) = delete;
    DTCWPTProcessor(DTCWPTProcessor&&) = delete;
    DTCWPTProcessor& operator=(DTCWPTProcessor&&) = delete;

    void prepareToPlay(double sampleRate, int maxBlockSize,
                       const TopologyConfig& config, int channelNum);

    void processBlock(juce::AudioBuffer<double>& buffer);

    int getLatency() const;

    /**
     * @brief Upper bound, in samples, of how long the output rings on after getLatency().
     *
     * Without band processing the engine is a pure delay of getLatency() samples. If a
     * BandProcessor changes subband coefficients, the response of each destination band
     * (analysis cascade, decimation, delay compensation, synthesis cascade) is no longer
     * cancelled by the other bands and spreads past that position. This value bounds how long
     * the output can keep going after the input has gone silent, for any processing that maps
     * zero subband input to zero output (i.e. the filter bank only; envelopes or other state
     * kept inside a BandProcessor are not included). Returns 0 before prepareToPlay().
     *
     * Derivation, for destination d at depth D with path nodes n_0 (root) ... n_{D-1}:
     *  - Stage j (0-based) filters with the analysis kernel a_j (h0 or h1 of n_j, chosen by the
     *    path bit) and the synthesis kernel s_j (g0 or g1). Decimating/upsampling by 2 at every
     *    stage puts stage j's taps 2^j input samples apart (noble identity), so the equivalent
     *    analysis kernel is a_0 * (a_1 upsampled by 2) * ... and the equivalent synthesis kernel
     *    is built the same way.
     *  - Let ea_j / es_j be the index of the last non-zero tap of a_j / s_j. Trailing zero taps
     *    (CDF 9/7 padding) do not extend the response; leading zero taps are a real delay and stay
     *    inside the index. The last non-zero tap of the equivalent kernels is
     *    Ea = sum_j ea_j * 2^j and Es = sum_j es_j * 2^j.
     *  - An input that ends at sample T makes the band coefficient with index m non-zero only while
     *    2^D * m - T <= Ea, i.e. up to m = floor((T + Ea) / 2^D). The band delay buffer then adds
     *    k = floor((maxDelay - pathDelay) / 2^D) band samples, and the synthesis cascade ends Es
     *    samples after the position 2^D * (m + k).
     *  - Relative to the unmodified output at T + maxDelay this ends at
     *    Ea + Es + 2^D * k - maxDelay - ((T + Ea) mod 2^D). The decimation phase term is largest
     *    (zero) for T = -Ea mod 2^D, which an input can always have, so it is dropped to get an
     *    upper bound that holds for every phase.
     *  - The result is the maximum over all destinations and both trees, never negative. The
     *    bound is exact for the filter-bank support; the part above -120 dB is shorter because the
     *    outer taps of the cascaded kernels are tiny.
     *
     * The value is relative to getLatency(): the FIFO delay of the block alignment is already
     * part of the latency and does not change the tail.
     */
    int getTailLengthSamples() const;

    /**
     * @brief Register a band processor. Takes ownership.
     *
     * If prepareToPlay() was already called, immediately calls prepare() on the processor.
     * Must be called when audio is NOT running.
     *
     * @param processor The BandProcessor to register (ownership transferred)
     */
    void setBandProcessor(std::unique_ptr<BandProcessor> processor);

    /**
     * @brief Feed sidechain audio for parallel CWPT analysis.
     *
     * Must be called with the same sample count as the main processBlock() call.
     * Channel count is adjusted to match main input (duplicate last / truncate).
     * Must be called BEFORE processBlock() for the same block.
     *
     * @param sidechainBuffer The sidechain audio buffer to process
     */
    void processSidechain(juce::AudioBuffer<double>& sidechainBuffer);

#if DTCWPT_ENABLE_TEST_SEAMS
    enum class PrepareCandidateFailpoint {
        none,
        beforeCommit,
    };

    void setPrepareCandidateFailpointForTesting(PrepareCandidateFailpoint failpoint) noexcept;
    void setAnalysisTraceSinkForTesting(AnalysisTraceSink* sink) noexcept;
#endif

private:
    // Configuration
    double sampleRate_;
    int maxBlockSize_;
    int channelNum_;

    // Topology
    std::unique_ptr<TopologyPlanner> topology_;
    std::vector<int> analysisOrder_;
    std::vector<int> synthesisOrder_;
    std::vector<int> destinations_;

    // Node storage per channel
    std::vector<AnalysisNodeGroup> analysisNodesRe_;
    std::vector<AnalysisNodeGroup> analysisNodesIm_;
    std::vector<SynthesisNodeGroup> synthesisNodesRe_;
    std::vector<SynthesisNodeGroup> synthesisNodesIm_;

    // Analysis node IDs (shared across channels)
    std::vector<int> analysisNodeIds_;
    AnalysisSchedulerMetadata analysisSchedulerMetadata_;
    std::vector<AnalysisRuntimeState> analysisRuntimeStatesRe_;
    std::vector<AnalysisRuntimeState> analysisRuntimeStatesIm_;

    // Synthesis node IDs (shared across channels)
    std::vector<int> synthesisNodeIds_;

    // Delay buffers per channel, indexed by destination order
    std::vector<std::vector<DelayBuffer>> delayBuffersRe_;
    std::vector<std::vector<DelayBuffer>> delayBuffersIm_;

    // Fixed-size work buffers mapping node IDs directly to indices for O(1) access
    std::vector<double> analysisWorkBuffer_;
    std::vector<char> analysisActiveFlags_;

    // Per-channel sparse result buffers indexed by node ID [channel][nodeId]
    std::vector<std::vector<std::vector<double>>> resultsRe_;
    std::vector<std::vector<std::vector<double>>> resultsIm_;
    std::vector<std::vector<size_t>> cursorsRe_;
    std::vector<std::vector<size_t>> cursorsIm_;

    // Per-channel temporary buffer for delay processing (pre-allocated)
    std::vector<std::vector<double>> delayTempBuffer_;

    // Temporary buffers for block/slice processing
    std::vector<double> inputBlockBuffer_;
    std::vector<double> delaySliceBuffer_;

    // Latency
    int maxDelay_;

    /// Filter-bank tail beyond getLatency() (see getTailLengthSamples())
    int tailLengthSamples_;

    // --- Block alignment ring buffer ---
    /// Alignment unit = 2^actualDepth; internal block must be a multiple of this.
    size_t alignUnit_;
    /// Internal processing block size = ceil(maxBlockSize / alignUnit) * alignUnit
    size_t internalBlockSize_;
    /// Per-channel input accumulation FIFO [ch][0..2*internalBlockSize_)
    std::vector<std::vector<double>> inputFifo_;
    /// Samples currently in each channel's input FIFO
    std::vector<size_t> inputFifoFill_;
    /// Per-channel output FIFO [ch][0..2*internalBlockSize_)
    std::vector<std::vector<double>> outputFifo_;
    /// Samples available for reading in each channel's output FIFO
    std::vector<size_t> outputFifoFill_;
    /// Read cursor for each channel's output FIFO
    std::vector<size_t> outputFifoRead_;

    // --- BandProcessor integration ---
    /// Optional band processor for subband modification
    std::unique_ptr<BandProcessor> bandProcessor_;
    /// True if prepareToPlay() has been called
    bool prepared_ = false;

    // --- Sidechain infrastructure ---
    /// Sidechain analysis nodes per channel (mirrors main analysis nodes)
    std::vector<AnalysisNodeGroup> scAnalysisNodesRe_;
    std::vector<AnalysisNodeGroup> scAnalysisNodesIm_;
    std::vector<AnalysisRuntimeState> scAnalysisRuntimeStatesRe_;
    std::vector<AnalysisRuntimeState> scAnalysisRuntimeStatesIm_;
    /// Sidechain delay buffers per channel (mirrors main delay buffers)
    std::vector<std::vector<DelayBuffer>> scDelayBuffersRe_;
    std::vector<std::vector<DelayBuffer>> scDelayBuffersIm_;
    /// Per-channel sidechain result buffers [channel][nodeId]
    std::vector<std::vector<std::vector<double>>> scResultsRe_;
    std::vector<std::vector<std::vector<double>>> scResultsIm_;
    std::vector<std::vector<size_t>> scCursorsRe_;
    std::vector<std::vector<size_t>> scCursorsIm_;
    /// Per-channel sidechain work buffers
    std::vector<double> scAnalysisWorkBuffer_;
    std::vector<char> scAnalysisActiveFlags_;
    std::vector<std::vector<double>> scDelayTempBuffer_;
    std::vector<double> scInputBlockBuffer_;
    std::vector<double> scDelaySliceBuffer_;
    /// Sidechain FIFO (mirrors main input FIFO)
    std::vector<std::vector<double>> scInputFifo_;
    std::vector<size_t> scInputFifoFill_;
    /// Sidechain state
    bool sidechainActive_ = false;
    bool sidechainProcessedThisBlock_ = false;
    /// Expected sample count for sidechain buffer validation
    int expectedSidechainSamples_ = 0;

    // --- Pre-allocated sidechain adjusted channels buffer ---
    std::vector<std::vector<double>> scAdjustedChannels_;

    // --- Pre-allocated processBlock buffers ---
    std::vector<std::vector<double>> alignedInputBuffers_;
    std::vector<std::vector<double>> alignedSidechainBuffers_;
    juce::AudioBuffer<double> alignedOutputBuffer_;

    // --- Pre-allocated BandData buffer ---
    BandData cachedBandData_;

    // --- Pre-allocated channel tracking buffer (RT-safe, avoids per-block alloc) ---
    std::vector<char> channelHasData_;  // use char instead of bool to avoid bit-packed specialization
    std::vector<char> scChannelHasData_;  // use char instead of bool to avoid bit-packed specialization

#if DTCWPT_ENABLE_TEST_SEAMS
    PrepareCandidateFailpoint prepareCandidateFailpoint_ = PrepareCandidateFailpoint::none;
    AnalysisTraceSink* analysisTraceSink_ = nullptr;
#endif

    // Helper methods
    /// Analysis + delay compensation phase for one channel.
    void processChannelAnalysisAndDelay(int ch, const std::vector<double>& inputBlock);

    /// Sidechain analysis phase for one channel (no delay compensation - sidechain is control signal).
    void processSidechainAnalysisAndDelay(int ch, const std::vector<double>& inputBlock);

    /// Synthesis + output averaging phase for one channel.
    void processChannelSynthesisAndAverage(int ch, juce::AudioBuffer<double>& output);

    void analysisProcess(const std::vector<double>& inputBlock,
                         AnalysisNodeGroup& nodeGroup,
                         const AnalysisSchedulerMetadata& metadata,
                         std::vector<std::vector<double>>& results,
                         std::vector<size_t>& cursors,
                         AnalysisRuntimeState& runtime,
                         AnalysisPathId path,
                         int channel);

    void synthesisProcess(std::vector<std::vector<double>>& analysedData,
                          SynthesisNodeGroup& nodeGroup,
                          const std::vector<int>& nodeIds,
                          std::vector<size_t>& cursors);

    static int getNodeLevel(int nodeId);
    
    int calculateDelayForNode(int targetIdx, const SynthesisNodeGroup& nodeGroup);
};

} // namespace dtcwpt
