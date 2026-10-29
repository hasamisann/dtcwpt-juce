#pragma once

#include "dtcwpt_analysis_node.h"
#include "dtcwpt_analysis_scheduler.h"
#include "dtcwpt_synthesis_node.h"
#include "dtcwpt_delay_buffer.h"
#include "dtcwpt_topology_limits.h"
#include "dtcwpt_topology_planner.h"
#include "dtcwpt_band_processor.h"
#include "dtcwpt_frequency_mapping.h"
#include "dtcwpt_filter_structs.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstddef>
#include <cstdint>
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
 * @brief Static description of one synthesis node for tick-by-tick scheduling.
 */
struct SynthesisTickPlan {
    size_t nodeIndex = 0;     ///< Index into SynthesisNodeGroup::nodes
    int nodeId = 0;           ///< Node ID (children are 2*nodeId and 2*nodeId+1)
    bool lowIsLeaf = false;   ///< Low child is a destination band (otherwise another synthesis node)
    bool highIsLeaf = false;  ///< High child is a destination band
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

    /**
     * @brief Processes one host block in place.
     *
     * The engine is streaming: every input sample is analysed as it arrives, the
     * BandProcessor is invoked once per call with the subband samples that became
     * available during the call (the count differs per band and may be zero), and
     * the output samples of the same time span are synthesised. The work per call
     * is therefore proportional to the block length, independent of the topology
     * depth and of the host's maximum block size.
     *
     * Blocks longer than the maxBlockSize given to prepareToPlay() are processed in
     * chunks of at most maxBlockSize samples (one BandProcessor call per chunk).
     */
    void processBlock(juce::AudioBuffer<double>& buffer) noexcept;

    /**
     * @brief Latency in samples between input and output.
     *
     * Equal to the largest filter-path delay of the topology; it does not depend
     * on the host block size.
     */
    int getLatency() const;
    int getTailLengthSamples() const;
    std::vector<NormalizedFrequencyRange> getDestinationFrequencyRanges() const;
    /** Clear all stream state without allocating; do not call concurrently with audio. */
    void reset();
    /** Process any main/sidechain length together, including oversized host blocks. */
    void processBlock(juce::AudioBuffer<double>& buffer, const juce::AudioBuffer<double>* sidechain) noexcept;

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
     * Must be called with the same sample count as the main processBlock() call,
     * which must not exceed the maxBlockSize given to prepareToPlay() (a longer
     * sidechain block is ignored). Channel count is adjusted to match main input
     * (duplicate last / truncate). Must be called BEFORE processBlock() for the
     * same block.
     *
     * @param sidechainBuffer The sidechain audio buffer to process
     */
    void processSidechain(juce::AudioBuffer<double>& sidechainBuffer) noexcept;

#if DTCWPT_ENABLE_TEST_SEAMS
    enum class PrepareCandidateFailpoint {
        none,
        beforeCommit,
        beforeCommitBadAlloc,
    };

    enum class AnalysisFaultForTesting { none, internalNodeLookup, cursorOverflow };
    void setAnalysisFaultForTesting(AnalysisFaultForTesting fault) noexcept { analysisFaultForTesting_ = fault; }
    void setPrepareCandidateFailpointForTesting(PrepareCandidateFailpoint failpoint) noexcept;
    void setAnalysisTraceSinkForTesting(AnalysisTraceSink* sink) noexcept;

    /**
     * @brief Cumulative amount of filter-bank work done since prepareToPlay().
     *
     * Used by tests to measure how evenly the work is spread over host callbacks.
     */
    struct WorkCounters {
        std::uint64_t analysisArrivals = 0;       ///< AnalysisNode::processArrival() calls (all paths)
        std::uint64_t synthesisOutputSamples = 0; ///< Samples produced by synthesis nodes (Re and Im)
    };

    WorkCounters getWorkCountersForTesting() const noexcept;
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

    // Temporary buffers for delay processing (pre-allocated, sized for the widest band)
    std::vector<std::vector<double>> delayTempBuffer_;
    std::vector<double> delaySliceBuffer_;

    // Latency
    int maxDelay_;
    int tailLengthSamples_ = 0;

    // --- Streaming state ---
    /// Number of input samples consumed (= number of output ticks synthesised) since prepareToPlay()
    std::uint64_t streamPos_ = 0;
    /// Deepest level of an internal (synthesis) node
    int maxSynthesisLevel_ = 0;
    /// Synthesis nodes grouped by level; plan.nodeIndex points into synthesisNodes{Re,Im}_[ch].nodes
    std::vector<std::vector<SynthesisTickPlan>> synthesisLevelPlans_;
    /// Latest output of every synthesis node [channel][nodeId]
    std::vector<std::vector<double>> synthesisOutputRe_;
    std::vector<std::vector<double>> synthesisOutputIm_;
    /// Next unread sample of every destination band within the current chunk [channel][nodeId]
    std::vector<std::vector<size_t>> leafReadCursors_;
    /// Silent input used for channels the host buffer does not provide, and sink for unused output
    std::vector<double> silenceBuffer_;
    std::vector<double> discardBuffer_;

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
    std::vector<double> scDelaySliceBuffer_;
    /// Number of input samples the sidechain analysis has consumed (may lag streamPos_ when no sidechain was fed)
    std::uint64_t scStreamPos_ = 0;
    /// Sidechain state
    bool sidechainActive_ = false;
    bool sidechainProcessedThisBlock_ = false;
    /// Expected sample count for sidechain buffer validation
    int expectedSidechainSamples_ = 0;

    // --- Pre-allocated sidechain adjusted channels buffer ---
    std::vector<std::vector<double>> scAdjustedChannels_;

    // --- Pre-allocated BandData buffer ---
    BandData cachedBandData_;

#if DTCWPT_ENABLE_TEST_SEAMS
    AnalysisFaultForTesting analysisFaultForTesting_ = AnalysisFaultForTesting::none;
    PrepareCandidateFailpoint prepareCandidateFailpoint_ = PrepareCandidateFailpoint::none;
    AnalysisTraceSink* analysisTraceSink_ = nullptr;
    WorkCounters workCounters_;
#endif

    void processBlockInternal(juce::AudioBuffer<double>& buffer, const juce::AudioBuffer<double>* directSidechain) noexcept;

    // Helper methods
    /// Analysis + delay compensation phase for one channel and one chunk of numSamples input samples.
    void processChannelAnalysisAndDelay(int ch, const double* input, size_t numSamples);

    /// Sidechain analysis phase for one channel (no delay compensation - sidechain is control signal).
    void processSidechainAnalysisAndDelay(int ch, const double* input, size_t numSamples);

    /// Synthesises numSamples output ticks starting at streamPos_ for one channel and averages the Re/Im trees.
    void processChannelSynthesisAndAverage(int ch, double* output, size_t numSamples);

    /// Makes the sidechain analysis decimation phase match the main stream after a gap without sidechain input.
    void alignSidechainPhase() noexcept;

    /// Fills cachedBandData_ for the current chunk. Returns true if any band holds samples.
    bool prepareBandData(bool hasSidechain);

    void analysisProcess(const double* input,
                         size_t numSamples,
                         AnalysisNodeGroup& nodeGroup,
                         const AnalysisSchedulerMetadata& metadata,
                         std::vector<std::vector<double>>& results,
                         std::vector<size_t>& cursors,
                         AnalysisRuntimeState& runtime,
                         AnalysisPathId path,
                         int channel);

    static int getNodeLevel(int nodeId);
    
    int calculateDelayForNode(int targetIdx, const SynthesisNodeGroup& nodeGroup);
};

} // namespace dtcwpt
