#include "dtcwpt_processor.h"

#include "dtcwpt_topology_planner.h"
#include "dtcwpt_filter_coeffs.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace dtcwpt {

DTCWPTProcessor::DTCWPTProcessor()
    : sampleRate_(0.0)
    , maxBlockSize_(0)
    , channelNum_(0)
    , topology_(nullptr)
    , analysisOrder_()
    , synthesisOrder_()
    , destinations_()
    , analysisNodesRe_()
    , analysisNodesIm_()
    , synthesisNodesRe_()
    , synthesisNodesIm_()
    , analysisNodeIds_()
    , synthesisNodeIds_()
    , delayBuffersRe_()
    , delayBuffersIm_()
    , analysisWorkBuffer_()
    , analysisActiveFlags_()
    , resultsRe_()
    , resultsIm_()
    , cursorsRe_()
    , cursorsIm_()
    , delayTempBuffer_()
    , inputBlockBuffer_()
    , delaySliceBuffer_()
    , maxDelay_(0)
    , alignUnit_(1)
    , internalBlockSize_(0)
    , inputFifo_()
    , inputFifoFill_()
    , outputFifo_()
    , outputFifoFill_()
    , outputFifoRead_()
    , bandProcessor_(nullptr)
    , prepared_(false)
    , scAnalysisNodesRe_()
    , scAnalysisNodesIm_()
    , scDelayBuffersRe_()
    , scDelayBuffersIm_()
    , scResultsRe_()
    , scResultsIm_()
    , scCursorsRe_()
    , scCursorsIm_()
    , scAnalysisWorkBuffer_()
    , scAnalysisActiveFlags_()
    , scDelayTempBuffer_()
    , scInputBlockBuffer_()
    , scDelaySliceBuffer_()
    , scInputFifo_()
    , scInputFifoFill_()
    , sidechainActive_(false)
    , sidechainProcessedThisBlock_(false)
    , expectedSidechainSamples_(0) {
}

void DTCWPTProcessor::prepareToPlay(double sampleRate, int maxBlockSize,
                                    const TopologyConfig& config, int channelNum) {
    // Validate parameters
    if (sampleRate <= 0.0) {
        throw std::invalid_argument("Sample rate must be positive");
    }
    if (maxBlockSize <= 0) {
        throw std::invalid_argument("Block size must be positive");
    }
    if (channelNum < 1 || channelNum > 2) {
        throw std::invalid_argument("Channel count must be 1 or 2");
    }
    if (config.destinations.empty()) {
        throw std::invalid_argument("At least one destination required");
    }

    // Store configuration
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    channelNum_ = channelNum;
    // Create topology planner
    topology_ = std::make_unique<TopologyPlanner>(config.destinations);
    auto plan = topology_->getPlan();
    analysisOrder_ = std::move(plan.first);
    synthesisOrder_ = std::move(plan.second);
    destinations_ = topology_->destinations;

    // Clear all per-channel data
    analysisNodesRe_.clear();
    analysisNodesIm_.clear();
    synthesisNodesRe_.clear();
    synthesisNodesIm_.clear();
    analysisNodeIds_.clear();
    synthesisNodeIds_.clear();
    delayBuffersRe_.clear();
    delayBuffersIm_.clear();

    // Resize per-channel data structures
    analysisNodesRe_.resize(static_cast<size_t>(channelNum));
    analysisNodesIm_.resize(static_cast<size_t>(channelNum));
    synthesisNodesRe_.resize(static_cast<size_t>(channelNum));
    synthesisNodesIm_.resize(static_cast<size_t>(channelNum));
    delayBuffersRe_.resize(static_cast<size_t>(channelNum));
    delayBuffersIm_.resize(static_cast<size_t>(channelNum));

    // =========================================================================
    // 1. Analysis Nodes Initialization
    // =========================================================================

    // Root node (idx=1)
    for (int ch = 0; ch < channelNum; ++ch) {
        AnalysisNode nodeRe(filters::CDF_RE, true);
        AnalysisNode nodeIm(filters::CDF_IM, true);
        analysisNodesRe_[ch].nodes.push_back(std::move(nodeRe));
        analysisNodesIm_[ch].nodes.push_back(std::move(nodeIm));
    }
    analysisNodeIds_.push_back(1);

    // Non-root analysis nodes
    for (int idx : analysisOrder_) {
        if (idx == 1) continue;
        analysisNodeIds_.push_back(idx);

        if (idx % 2 == 1) {
            // Odd: PACKET filters (same for Re and Im)
            for (int ch = 0; ch < channelNum; ++ch) {
                AnalysisNode nodeRe(filters::PACKET, false);
                AnalysisNode nodeIm(filters::PACKET, false);
                analysisNodesRe_[ch].nodes.push_back(std::move(nodeRe));
                analysisNodesIm_[ch].nodes.push_back(std::move(nodeIm));
            }
        } else {
            // Even: QSHIFT14 (Re vs Im differ)
            for (int ch = 0; ch < channelNum; ++ch) {
                AnalysisNode nodeRe(filters::QSHIFT14_RE, false);
                AnalysisNode nodeIm(filters::QSHIFT14_IM, false);
                analysisNodesRe_[ch].nodes.push_back(std::move(nodeRe));
                analysisNodesIm_[ch].nodes.push_back(std::move(nodeIm));
            }
        }
    }

    // Set IDs for each channel's node group
    for (int ch = 0; ch < channelNum; ++ch) {
        analysisNodesRe_[ch].ids = analysisNodeIds_;
        analysisNodesIm_[ch].ids = analysisNodeIds_;
    }

    // =========================================================================
    // 2. Synthesis Nodes Initialization
    // =========================================================================
    for (int idx : synthesisOrder_) {
        synthesisNodeIds_.push_back(idx);

        if (idx == 1) {
            // Root: CDF synthesis filters
            for (int ch = 0; ch < channelNum; ++ch) {
                SynthesisNode nodeRe(filters::CDF_RE, true);
                SynthesisNode nodeIm(filters::CDF_IM, true);
                synthesisNodesRe_[ch].nodes.push_back(std::move(nodeRe));
                synthesisNodesIm_[ch].nodes.push_back(std::move(nodeIm));
            }
        } else if (idx % 2 == 1) {
            // Odd: PACKET synthesis filters
            for (int ch = 0; ch < channelNum; ++ch) {
                SynthesisNode nodeRe(filters::PACKET, false);
                SynthesisNode nodeIm(filters::PACKET, false);
                synthesisNodesRe_[ch].nodes.push_back(std::move(nodeRe));
                synthesisNodesIm_[ch].nodes.push_back(std::move(nodeIm));
            }
        } else {
            // Even: QSHIFT14 synthesis filters
            for (int ch = 0; ch < channelNum; ++ch) {
                SynthesisNode nodeRe(filters::QSHIFT14_RE, false);
                SynthesisNode nodeIm(filters::QSHIFT14_IM, false);
                synthesisNodesRe_[ch].nodes.push_back(std::move(nodeRe));
                synthesisNodesIm_[ch].nodes.push_back(std::move(nodeIm));
            }
        }
    }

    for (int ch = 0; ch < channelNum; ++ch) {
        synthesisNodesRe_[ch].ids = synthesisNodeIds_;
        synthesisNodesIm_[ch].ids = synthesisNodeIds_;
    }

    // =========================================================================
    // 3. Delay Buffer Initialization
    // =========================================================================
    maxDelay_ = 0;
    std::vector<int> pathDelaysRe(destinations_.size());
    std::vector<int> pathDelaysIm(destinations_.size());
    
    // Calculate max delay across all destination paths
    for (size_t i = 0; i < destinations_.size(); ++i) {
        int dest = destinations_[i];
        int dRe = calculateDelayForNode(dest, synthesisNodesRe_[0]);
        int dIm = calculateDelayForNode(dest, synthesisNodesIm_[0]);
        pathDelaysRe[i] = dRe;
        pathDelaysIm[i] = dIm;
        maxDelay_ = std::max(maxDelay_, std::max(dRe, dIm));
    }

    // Create delay buffers per destination (in order)
    for (size_t i = 0; i < destinations_.size(); ++i) {
        int dest = destinations_[i];
        int depth = getNodeLevel(dest);
        int rateFactor = 1 << depth;

        // Re
        int diffReInput = maxDelay_ - pathDelaysRe[i];
        int diffReSubband = diffReInput / rateFactor;

        // Im
        int diffImInput = maxDelay_ - pathDelaysIm[i];
        int diffImSubband = diffImInput / rateFactor;

        for (int ch = 0; ch < channelNum; ++ch) {
            delayBuffersRe_[ch].emplace_back(diffReSubband, maxBlockSize);
            delayBuffersIm_[ch].emplace_back(diffImSubband, maxBlockSize);
        }
    }

    // =========================================================================
    // 4. Result Buffers Initialization (per-channel)
    // =========================================================================
    // Compute internalBlockSize_ here (before result buffer allocation) so that
    // buffers are large enough for processChannelAligned, which processes
    // internalBlockSize_ samples, not maxBlockSize samples.
    {
        size_t maxDepth = 0;
        for (int dest : destinations_)
            maxDepth = std::max(maxDepth, static_cast<size_t>(getNodeLevel(dest)));
        alignUnit_ = static_cast<size_t>(1) << maxDepth;
        const size_t n = static_cast<size_t>(maxBlockSize);
        internalBlockSize_ = ((n + alignUnit_ - 1) / alignUnit_) * alignUnit_;
    }

    const size_t MAX_NODE_ID = TopologyPlanner::MAX_SIZE;
    
    // Per-channel result buffers: [channel][nodeId]
    resultsRe_.clear();
    resultsIm_.clear();
    resultsRe_.resize(static_cast<size_t>(channelNum));
    resultsIm_.resize(static_cast<size_t>(channelNum));
    cursorsRe_.resize(static_cast<size_t>(channelNum));
    cursorsIm_.resize(static_cast<size_t>(channelNum));
    
    for (int ch = 0; ch < channelNum; ++ch) {
        resultsRe_[ch].resize(MAX_NODE_ID);
        resultsIm_[ch].resize(MAX_NODE_ID);
        cursorsRe_[ch].assign(MAX_NODE_ID, 0);
        cursorsIm_[ch].assign(MAX_NODE_ID, 0);
        
        // Allocate output size based on internalBlockSize_ (not maxBlockSize)
        for (int target : analysisOrder_) {
            int depth = getNodeLevel(target);
            size_t outputSize = internalBlockSize_ >> static_cast<size_t>(depth);
            if (outputSize < 1) outputSize = 1;
            resultsRe_[ch][target].assign(outputSize, 0.0);
            resultsIm_[ch][target].assign(outputSize, 0.0);
        }
        for (int target : destinations_) {
            int depth = getNodeLevel(target);
            size_t outputSize = internalBlockSize_ >> static_cast<size_t>(depth);
            if (outputSize < 1) outputSize = 1;
            resultsRe_[ch][target].assign(outputSize, 0.0);
            resultsIm_[ch][target].assign(outputSize, 0.0);
        }
    }

    // Note: synthesisOrder_ loop removed — analysisOrder == synthesisOrder (same set).

    // =========================================================================
    // 5. Work Buffers
    // =========================================================================
    analysisWorkBuffer_.assign(MAX_NODE_ID, 0.0);
    analysisActiveFlags_.assign(MAX_NODE_ID, 0);

    // Per-channel delay temp buffers - pre-allocate to max size for RT safety
    delayTempBuffer_.clear();
    delayTempBuffer_.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        delayTempBuffer_[ch].resize(static_cast<size_t>(internalBlockSize_));
    }
    inputBlockBuffer_.resize(static_cast<size_t>(internalBlockSize_));
    delaySliceBuffer_.resize(static_cast<size_t>(internalBlockSize_));

    // =========================================================================
    // 6. Block alignment ring buffers
    // =========================================================================

    // Allocate FIFOs.
    // inputFifo : internalBlockSize_ * 2 is sufficient (1 block of headroom).
    // outputFifo: must hold up to (maxDelay_ + 2 * internalBlockSize_) output samples,
    //             because the host may feed up to getLatency() = maxDelay_ + internalBlockSize_
    //             additional zero-pad samples after the signal ends, triggering up to
    //             ceil(maxDelay_ / internalBlockSize_) + 1 extra processing blocks.
    {
        const size_t inputFifoSize  = internalBlockSize_ * 2;
        const size_t outputFifoSize = static_cast<size_t>(maxDelay_) + internalBlockSize_ * 4;
        inputFifo_.assign(static_cast<size_t>(channelNum),
                          std::vector<double>(inputFifoSize, 0.0));
        outputFifo_.assign(static_cast<size_t>(channelNum),
                           std::vector<double>(outputFifoSize, 0.0));
        inputFifoFill_.assign(static_cast<size_t>(channelNum), 0);
        outputFifoFill_.assign(static_cast<size_t>(channelNum), 0);
        outputFifoRead_.assign(static_cast<size_t>(channelNum), 0);

        // Pre-fill output FIFO with internalBlockSize_ zeros so that the startup
        // silence period is exactly internalBlockSize_ samples regardless of the
        // host block size.  getLatency() = maxDelay_ + internalBlockSize_ then
        // precisely represents the end-to-end latency.
        for (int ch = 0; ch < channelNum; ++ch) {
            outputFifoFill_[static_cast<size_t>(ch)] = internalBlockSize_;
        }
    }

    // =========================================================================
    // 7. Sidechain infrastructure (pre-allocated, mirrors main input path)
    // =========================================================================

    // Clear and resize per-channel sidechain data structures
    scAnalysisNodesRe_.clear();
    scAnalysisNodesIm_.clear();
    scAnalysisNodesRe_.resize(static_cast<size_t>(channelNum));
    scAnalysisNodesIm_.resize(static_cast<size_t>(channelNum));
    scDelayBuffersRe_.clear();
    scDelayBuffersIm_.clear();
    scDelayBuffersRe_.resize(static_cast<size_t>(channelNum));
    scDelayBuffersIm_.resize(static_cast<size_t>(channelNum));

    // Initialize sidechain analysis nodes (same as main)
    // Root node (idx=1)
    for (int ch = 0; ch < channelNum; ++ch) {
        AnalysisNode nodeRe(filters::CDF_RE, true);
        AnalysisNode nodeIm(filters::CDF_IM, true);
        scAnalysisNodesRe_[ch].nodes.push_back(std::move(nodeRe));
        scAnalysisNodesIm_[ch].nodes.push_back(std::move(nodeIm));
    }
    scAnalysisNodesRe_[0].ids.push_back(1);
    scAnalysisNodesIm_[0].ids.push_back(1);

    // Non-root analysis nodes (same logic as main)
    for (int idx : analysisOrder_) {
        if (idx == 1) continue;

        if (idx % 2 == 1) {
            // Odd: PACKET filters
            for (int ch = 0; ch < channelNum; ++ch) {
                AnalysisNode nodeRe(filters::PACKET, false);
                AnalysisNode nodeIm(filters::PACKET, false);
                scAnalysisNodesRe_[ch].nodes.push_back(std::move(nodeRe));
                scAnalysisNodesIm_[ch].nodes.push_back(std::move(nodeIm));
            }
        } else {
            // Even: QSHIFT14 (Re vs Im differ)
            for (int ch = 0; ch < channelNum; ++ch) {
                AnalysisNode nodeRe(filters::QSHIFT14_RE, false);
                AnalysisNode nodeIm(filters::QSHIFT14_IM, false);
                scAnalysisNodesRe_[ch].nodes.push_back(std::move(nodeRe));
                scAnalysisNodesIm_[ch].nodes.push_back(std::move(nodeIm));
            }
        }
    }

    // Set IDs for each channel's sidechain node group
    for (int ch = 0; ch < channelNum; ++ch) {
        scAnalysisNodesRe_[ch].ids = analysisNodeIds_;
        scAnalysisNodesIm_[ch].ids = analysisNodeIds_;
    }

    // Initialize sidechain delay buffers (same delays as main)
    for (size_t i = 0; i < destinations_.size(); ++i) {
        int dest = destinations_[i];
        int depth = getNodeLevel(dest);
        int rateFactor = 1 << depth;

        int diffReInput = maxDelay_ - pathDelaysRe[i];
        int diffReSubband = diffReInput / rateFactor;
        int diffImInput = maxDelay_ - pathDelaysIm[i];
        int diffImSubband = diffImInput / rateFactor;

        for (int ch = 0; ch < channelNum; ++ch) {
            scDelayBuffersRe_[ch].emplace_back(diffReSubband, maxBlockSize);
            scDelayBuffersIm_[ch].emplace_back(diffImSubband, maxBlockSize);
        }
    }

    // Initialize per-channel sidechain result buffers [channel][nodeId]
    scResultsRe_.clear();
    scResultsIm_.clear();
    scResultsRe_.resize(static_cast<size_t>(channelNum));
    scResultsIm_.resize(static_cast<size_t>(channelNum));
    scCursorsRe_.resize(static_cast<size_t>(channelNum));
    scCursorsIm_.resize(static_cast<size_t>(channelNum));

    for (int ch = 0; ch < channelNum; ++ch) {
        scResultsRe_[ch].resize(MAX_NODE_ID);
        scResultsIm_[ch].resize(MAX_NODE_ID);
        scCursorsRe_[ch].assign(MAX_NODE_ID, 0);
        scCursorsIm_[ch].assign(MAX_NODE_ID, 0);
        
        for (int target : analysisOrder_) {
            int depth = getNodeLevel(target);
            size_t outputSize = internalBlockSize_ >> static_cast<size_t>(depth);
            if (outputSize < 1) outputSize = 1;
            scResultsRe_[ch][target].assign(outputSize, 0.0);
            scResultsIm_[ch][target].assign(outputSize, 0.0);
        }
        for (int target : destinations_) {
            int depth = getNodeLevel(target);
            size_t outputSize = internalBlockSize_ >> static_cast<size_t>(depth);
            if (outputSize < 1) outputSize = 1;
            scResultsRe_[ch][target].assign(outputSize, 0.0);
            scResultsIm_[ch][target].assign(outputSize, 0.0);
        }
    }

    // Initialize sidechain work buffers
    scAnalysisWorkBuffer_.assign(MAX_NODE_ID, 0.0);
    scAnalysisActiveFlags_.assign(MAX_NODE_ID, 0);
    scDelayTempBuffer_.clear();
    scDelayTempBuffer_.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        scDelayTempBuffer_[ch].resize(static_cast<size_t>(internalBlockSize_));
    }
    scInputBlockBuffer_.resize(static_cast<size_t>(internalBlockSize_));
    scDelaySliceBuffer_.resize(static_cast<size_t>(internalBlockSize_));

    // Initialize sidechain FIFO
    scInputFifo_.assign(static_cast<size_t>(channelNum),
                        std::vector<double>(internalBlockSize_ * 2, 0.0));
    scInputFifoFill_.assign(static_cast<size_t>(channelNum), 0);

    // Reset sidechain state
    sidechainActive_ = false;
    sidechainProcessedThisBlock_ = false;
    expectedSidechainSamples_ = 0;

    // =========================================================================
    // 8. Pre-allocate sidechain adjusted channels buffer
    // =========================================================================
    scAdjustedChannels_.resize(static_cast<size_t>(channelNum_));
    for (int ch = 0; ch < channelNum_; ++ch) {
        scAdjustedChannels_[ch].resize(static_cast<size_t>(internalBlockSize_));
    }

    // =========================================================================
    // 9. Pre-allocate processBlock internal buffers
    // =========================================================================
    alignedInputBuffers_.resize(static_cast<size_t>(channelNum_));
    alignedSidechainBuffers_.resize(static_cast<size_t>(channelNum_));
    for (int i = 0; i < channelNum_; ++i) {
        alignedInputBuffers_[i].resize(internalBlockSize_);
        alignedSidechainBuffers_[i].resize(internalBlockSize_);
    }
    alignedOutputBuffer_.setSize(1, static_cast<int>(internalBlockSize_));

    // =========================================================================
    // 10. Pre-allocate BandData buffer
    // =========================================================================
    cachedBandData_.bands.resize(static_cast<size_t>(channelNum_));
    cachedBandData_.sidechainBands.resize(static_cast<size_t>(channelNum_));
    for (int i = 0; i < channelNum_; ++i) {
        cachedBandData_.bands[i].resize(destinations_.size());
        cachedBandData_.sidechainBands[i].resize(destinations_.size());
    }

    // =========================================================================
    // 11. Pre-allocate channel tracking buffer (RT-safe)
    // =========================================================================
    channelHasData_.resize(static_cast<size_t>(channelNum_));
    scChannelHasData_.resize(static_cast<size_t>(channelNum_));

    // Reset band processor if exists (lifecycle hook)
    if (bandProcessor_) {
        bandProcessor_->reset();
    }

    // Mark as prepared
    prepared_ = true;

    // If BandProcessor was set before prepareToPlay(), prepare it now
    if (bandProcessor_) {
        size_t minDepth = static_cast<size_t>(getNodeLevel(destinations_[0]));
        for (int dest : destinations_) {
            const size_t d = static_cast<size_t>(getNodeLevel(dest));
            if (d < minDepth) minDepth = d;
        }
        int maxSamplesPerBand = static_cast<int>(internalBlockSize_ >> minDepth);
        if (maxSamplesPerBand < 1) maxSamplesPerBand = 1;

        bandProcessor_->prepare(sampleRate_, maxSamplesPerBand,
                                static_cast<int>(destinations_.size()), channelNum_);
    }
}

void DTCWPTProcessor::setBandProcessor(std::unique_ptr<BandProcessor> processor) {
    bandProcessor_ = std::move(processor);

    // Reset the newly swapped-in processor if plugin is already playing
    if (bandProcessor_ && prepared_) {
        bandProcessor_->reset();
    }

    // If already prepared, prepare the processor immediately
    if (prepared_ && bandProcessor_) {
        // Compute maxSamplesPerBand from internalBlockSize_ and MIN depth.
        // See comment above (prepareToPlay) for why minDepth is correct here.
        size_t minDepth = static_cast<size_t>(getNodeLevel(destinations_[0]));
        for (int dest : destinations_) {
            const size_t d = static_cast<size_t>(getNodeLevel(dest));
            if (d < minDepth) minDepth = d;
        }
        int maxSamplesPerBand = static_cast<int>(internalBlockSize_ >> minDepth);
        if (maxSamplesPerBand < 1) maxSamplesPerBand = 1;

        bandProcessor_->prepare(sampleRate_, maxSamplesPerBand,
                                static_cast<int>(destinations_.size()), channelNum_);
    }
}

void DTCWPTProcessor::processSidechain(juce::AudioBuffer<double>& sidechainBuffer) {
    const int numSamples = sidechainBuffer.getNumSamples();
    const int numChannels = sidechainBuffer.getNumChannels();

    // Store expected sample count for validation in processBlock()
    expectedSidechainSamples_ = numSamples;

    // Channel adjustment: match main input channel count
    // Use pre-allocated buffer (assumes numSamples <= maxBlockSize_)
    if (numChannels >= channelNum_) {
        // More or equal channels: copy first channelNum_ channels
        for (int ch = 0; ch < channelNum_; ++ch) {
            const double* src = sidechainBuffer.getReadPointer(ch);
            std::copy(src, src + numSamples, scAdjustedChannels_[ch].begin());
        }
    } else {
        // Fewer channels: duplicate last channel to fill remaining slots
        for (int ch = 0; ch < channelNum_; ++ch) {
            int srcCh = std::min(ch, numChannels - 1);
            const double* src = sidechainBuffer.getReadPointer(srcCh);
            std::copy(src, src + numSamples, scAdjustedChannels_[ch].begin());
        }
    }

    // Push adjusted sidechain samples into scInputFifo_
    const size_t need = static_cast<size_t>(numSamples);
    for (int ch = 0; ch < channelNum_; ++ch) {
        const double* src = scAdjustedChannels_[ch].data();
        size_t pushed = 0;

        while (pushed < need) {
            const size_t space = internalBlockSize_ - scInputFifoFill_[ch];
            const size_t toCopy = std::min(space, need - pushed);

            std::copy(src + pushed,
                      src + pushed + toCopy,
                      scInputFifo_[ch].begin() +
                          static_cast<ptrdiff_t>(scInputFifoFill_[ch]));
            scInputFifoFill_[ch] += toCopy;
            pushed += toCopy;

            // If FIFO is full, stop pushing (should not happen with correct usage)
            // The data remains in FIFO until processBlock extracts it
            if (scInputFifoFill_[ch] >= internalBlockSize_) {
                break;
            }
        }
    }

    sidechainActive_ = true;
    sidechainProcessedThisBlock_ = true;
}

// Calculate cumulative delay for a specific target node by traversing from root
int DTCWPTProcessor::calculateDelayForNode(int targetIdx, const SynthesisNodeGroup& nodeGroup) {
    // Build id to index map
    std::map<int, size_t> idToIdx;
    for (size_t i = 0; i < nodeGroup.ids.size(); ++i) {
        idToIdx[nodeGroup.ids[i]] = i;
    }
    
    int totalDelay = 0;
    int coef = 1;
    int currentIdx = 1;  // Start at root
    
    // Calculate bit length
    int numBits = 0;
    int temp = targetIdx;
    while (temp > 0) {
        temp >>= 1;
        ++numBits;
    }
    
    // Iterate from second-highest bit down to 0
    for (int bitPos = numBits - 2; bitPos >= 0; --bitPos) {
        int direction = (targetIdx >> bitPos) & 1;
        
        int delay = 0;
        auto it = idToIdx.find(currentIdx);
        if (it != idToIdx.end()) {
            const SynthesisNode& node = nodeGroup.nodes[it->second];
            if (direction == 0) {
                delay = node.getFilterLowDelay();
                currentIdx = currentIdx << 1;
            } else {
                delay = node.getFilterHighDelay();
                currentIdx = (currentIdx << 1) | 1;
            }
        } else {
            // Node not found in current path
            if (direction == 0) {
                currentIdx = currentIdx << 1;
            } else {
                currentIdx = (currentIdx << 1) | 1;
            }
        }

        totalDelay += delay * coef;
        coef <<= 1;
    }
    
    return totalDelay;
}

// Analysis + delay compensation phase for one channel.
void DTCWPTProcessor::processChannelAnalysisAndDelay(int ch, const std::vector<double>& inputBlock) {
    // Init cursors for all targets
    for (int target : analysisOrder_) {
        cursorsRe_[ch][static_cast<size_t>(target)] = 0;
        cursorsIm_[ch][static_cast<size_t>(target)] = 0;
    }
    for (int target : destinations_) {
        cursorsRe_[ch][static_cast<size_t>(target)] = 0;
        cursorsIm_[ch][static_cast<size_t>(target)] = 0;
    }

    // Analysis for both trees (pass per-channel results/cursors and shared work buffers)
    analysisProcess(inputBlock, analysisNodesRe_[ch], analysisNodeIds_, resultsRe_[ch], cursorsRe_[ch],
                    analysisWorkBuffer_, analysisActiveFlags_);
    analysisProcess(inputBlock, analysisNodesIm_[ch], analysisNodeIds_, resultsIm_[ch], cursorsIm_[ch],
                    analysisWorkBuffer_, analysisActiveFlags_);

    // Delay compensation — Re tree
    for (size_t di = 0; di < destinations_.size(); ++di) {
        int dest = destinations_[di];
        size_t subbandLen = cursorsRe_[ch][static_cast<size_t>(dest)];
        if (subbandLen > 0) {
            // Use pre-allocated buffers (no resize in hot path)
            std::copy_n(resultsRe_[ch][dest].begin(), static_cast<ptrdiff_t>(subbandLen), delaySliceBuffer_.begin());
            delayBuffersRe_[ch][di].process(delaySliceBuffer_, delayTempBuffer_[ch], subbandLen);
            std::copy_n(delayTempBuffer_[ch].begin(), static_cast<ptrdiff_t>(subbandLen), resultsRe_[ch][dest].begin());
        }
    }

    // Delay compensation — Im tree
    for (size_t di = 0; di < destinations_.size(); ++di) {
        int dest = destinations_[di];
        size_t subbandLen = cursorsIm_[ch][static_cast<size_t>(dest)];
        if (subbandLen > 0) {
            // Use pre-allocated buffers (no resize in hot path)
            std::copy_n(resultsIm_[ch][dest].begin(), static_cast<ptrdiff_t>(subbandLen), delaySliceBuffer_.begin());
            delayBuffersIm_[ch][di].process(delaySliceBuffer_, delayTempBuffer_[ch], subbandLen);
            std::copy_n(delayTempBuffer_[ch].begin(), static_cast<ptrdiff_t>(subbandLen), resultsIm_[ch][dest].begin());
        }
    }
}

// Sidechain analysis phase for one channel (no delay compensation - sidechain is control signal).
void DTCWPTProcessor::processSidechainAnalysisAndDelay(int ch, const std::vector<double>& inputBlock) {
    // Init cursors for all targets
    for (int target : analysisOrder_) {
        scCursorsRe_[ch][static_cast<size_t>(target)] = 0;
        scCursorsIm_[ch][static_cast<size_t>(target)] = 0;
    }
    for (int target : destinations_) {
        scCursorsRe_[ch][static_cast<size_t>(target)] = 0;
        scCursorsIm_[ch][static_cast<size_t>(target)] = 0;
    }

    // Analysis for both trees (pass per-channel results/cursors and per-channel work buffers)
    analysisProcess(inputBlock, scAnalysisNodesRe_[ch], analysisNodeIds_, scResultsRe_[ch], scCursorsRe_[ch],
                    scAnalysisWorkBuffer_, scAnalysisActiveFlags_);
    analysisProcess(inputBlock, scAnalysisNodesIm_[ch], analysisNodeIds_, scResultsIm_[ch], scCursorsIm_[ch],
                    scAnalysisWorkBuffer_, scAnalysisActiveFlags_);

    // Delay compensation — Re tree
    for (size_t di = 0; di < destinations_.size(); ++di) {
        int dest = destinations_[di];
        size_t subbandLen = scCursorsRe_[ch][static_cast<size_t>(dest)];
        if (subbandLen > 0) {
            // Use pre-allocated buffers (no resize in hot path)
            std::copy_n(scResultsRe_[ch][dest].begin(), static_cast<ptrdiff_t>(subbandLen), scDelaySliceBuffer_.begin());
            scDelayBuffersRe_[ch][di].process(scDelaySliceBuffer_, scDelayTempBuffer_[ch], subbandLen);
            std::copy_n(scDelayTempBuffer_[ch].begin(), static_cast<ptrdiff_t>(subbandLen), scResultsRe_[ch][dest].begin());
        }
    }

    // Delay compensation — Im tree
    for (size_t di = 0; di < destinations_.size(); ++di) {
        int dest = destinations_[di];
        size_t subbandLen = scCursorsIm_[ch][static_cast<size_t>(dest)];
        if (subbandLen > 0) {
            // Use pre-allocated buffers (no resize in hot path)
            std::copy_n(scResultsIm_[ch][dest].begin(), static_cast<ptrdiff_t>(subbandLen), scDelaySliceBuffer_.begin());
            scDelayBuffersIm_[ch][di].process(scDelaySliceBuffer_, scDelayTempBuffer_[ch], subbandLen);
            std::copy_n(scDelayTempBuffer_[ch].begin(), static_cast<ptrdiff_t>(subbandLen), scResultsIm_[ch][dest].begin());
        }
    }
}

// Synthesis + output averaging phase for one channel.
void DTCWPTProcessor::processChannelSynthesisAndAverage(int ch, juce::AudioBuffer<double>& output) {
    const int numSamples = output.getNumSamples();

    // Synthesis for both trees (pass per-channel results/cursors)
    synthesisProcess(resultsRe_[ch], synthesisNodesRe_[ch], synthesisNodeIds_, cursorsRe_[ch]);
    synthesisProcess(resultsIm_[ch], synthesisNodesIm_[ch], synthesisNodeIds_, cursorsIm_[ch]);

    // Output averaging
    double* writePtr = output.getWritePointer(0);
    for (int i = 0; i < numSamples; ++i) {
        double re = resultsRe_[ch][1][static_cast<size_t>(i)];
        double im = resultsIm_[ch][1][static_cast<size_t>(i)];
        writePtr[i] = (re + im) / 2.0;
    }
}

// Public entry point: accumulates input into FIFO, processes aligned blocks, drains output FIFO.
void DTCWPTProcessor::processBlock(juce::AudioBuffer<double>& buffer) {
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = std::min(buffer.getNumChannels(), channelNum_);

    if (numSamples == 0) return;

    // Validate sidechain sample count if sidechain was called
    // Instead of throwing (which is unsafe in audio thread), disable sidechain on mismatch
    if (sidechainProcessedThisBlock_ && numSamples != expectedSidechainSamples_) {        
        // Graceful fallback: disable sidechain for this block
        sidechainActive_ = false;
        sidechainProcessedThisBlock_ = false;
    }

    const size_t need = static_cast<size_t>(numSamples);

    // Track which channels have data ready (using pre-allocated buffer for RT safety)
    // Use channelHasData_ which was resized in prepareToPlay()
    std::fill(channelHasData_.begin(), channelHasData_.end(), static_cast<char>(0));

    bool allChannelsReady = true;
    
    // Step 1: push input samples into per-channel input FIFO
    for (int ch = 0; ch < numChannels; ++ch) {
        const double* src = buffer.getReadPointer(ch);
        size_t pushed = 0;

        while (pushed < need) {
            const size_t space   = internalBlockSize_ - inputFifoFill_[ch];
            const size_t toCopy  = std::min(space, need - pushed);

            std::copy(src + pushed,
                      src + pushed + toCopy,
                      inputFifo_[ch].begin() +
                          static_cast<ptrdiff_t>(inputFifoFill_[ch]));
            inputFifoFill_[ch] += toCopy;
            pushed              += toCopy;

            if (inputFifoFill_[ch] >= internalBlockSize_) {
                // Extract data from input FIFO into pre-allocated alignedInputBuffers_
                std::copy(inputFifo_[ch].begin(),
                              inputFifo_[ch].begin() + static_cast<ptrdiff_t>(internalBlockSize_),
                              alignedInputBuffers_[ch].begin());
                channelHasData_[ch] = static_cast<char>(1);
                inputFifoFill_[ch] = 0;
            }
        }
        
        // Check if this channel is ready
        if (!channelHasData_[ch]) {
            allChannelsReady = false;
        }
    }
    
    // Step 1.5: Extract sidechain FIFO data if sidechain is active
    if (sidechainActive_ && sidechainProcessedThisBlock_) {
        for (int ch = 0; ch < channelNum_; ++ch) {
            if (scInputFifoFill_[ch] >= internalBlockSize_) {
                std::copy(scInputFifo_[ch].begin(),
                          scInputFifo_[ch].begin() + static_cast<ptrdiff_t>(internalBlockSize_),
                          alignedSidechainBuffers_[ch].begin());
                scChannelHasData_[ch] = static_cast<char>(1);
                // Reset sidechain FIFO (simple single-block model)
                scInputFifoFill_[ch] = 0;
            }
        }
    }
    
    // Step 2: If all channels have data, process analysis + delay for all channels
    if (allChannelsReady) {
        for (int ch = 0; ch < channelNum_; ++ch) {
            processChannelAnalysisAndDelay(ch, alignedInputBuffers_[ch]);
        }
        
        // Step 2.25: Process sidechain analysis if active
        for (int ch = 0; ch < channelNum_; ++ch) {
            if (scChannelHasData_[ch]) {
                // Sidechain data is always available when sidechainActive_ and sidechainProcessedThisBlock_ are true
                processSidechainAnalysisAndDelay(ch, alignedSidechainBuffers_[ch]);
            }
        }
        
        // Step 2.5: Invoke BandProcessor if registered
        if (bandProcessor_) {
            // Reuse cached BandData (pre-allocated, no resize)
            cachedBandData_.numChannels = channelNum_;
            cachedBandData_.numBands = static_cast<int>(destinations_.size());
            cachedBandData_.hasSidechain = sidechainActive_ && sidechainProcessedThisBlock_;
            cachedBandData_.destinationIds = &destinations_;

            // Populate bands with pointers into per-channel resultsRe_/resultsIm_
            for (int ch = 0; ch < channelNum_; ++ch) {
                for (size_t b = 0; b < destinations_.size(); ++b) {
                    int dest = destinations_[b];
                    size_t numSamplesInBand = cursorsRe_[ch][dest];
                    cachedBandData_.bands[ch][b].re = resultsRe_[ch][dest].data();
                    cachedBandData_.bands[ch][b].im = resultsIm_[ch][dest].data();
                    cachedBandData_.bands[ch][b].numSamples = numSamplesInBand;
                }
            }

            // Populate sidechainBands if sidechain is active
            if (cachedBandData_.hasSidechain) {
                for (int ch = 0; ch < channelNum_; ++ch) {
                    for (size_t b = 0; b < destinations_.size(); ++b) {
                        int dest = destinations_[b];
                        size_t numSamplesInBand = scCursorsRe_[ch][dest];
                        cachedBandData_.sidechainBands[ch][b].re = scResultsRe_[ch][dest].data();
                        cachedBandData_.sidechainBands[ch][b].im = scResultsIm_[ch][dest].data();
                        cachedBandData_.sidechainBands[ch][b].numSamples = numSamplesInBand;
                    }
                }
            }

            // Call BandProcessor
            bandProcessor_->processAllBands(cachedBandData_);
        }
        
        // Step 3: Synthesis + output averaging for all channels
        for (int ch = 0; ch < channelNum_; ++ch) {
            // Use pre-allocated output buffer (clear it first)
            alignedOutputBuffer_.clear();
            
            processChannelSynthesisAndAverage(ch, alignedOutputBuffer_);

            // Append to output FIFO
            const size_t writePos = outputFifoRead_[ch] + outputFifoFill_[ch];
            assert(writePos + internalBlockSize_ <= outputFifo_[ch].size() &&
                   "output FIFO overflow during processBlock append");
            std::copy(alignedOutputBuffer_.getReadPointer(0),
                      alignedOutputBuffer_.getReadPointer(0) + static_cast<ptrdiff_t>(internalBlockSize_),
                      outputFifo_[ch].begin() +
                          static_cast<ptrdiff_t>(writePos));
            outputFifoFill_[ch] += internalBlockSize_;
        }
    }
    
    // Reset sidechain state for next block
    sidechainProcessedThisBlock_ = false;
    for (int ch = 0; ch < numChannels; ++ch) {
      scChannelHasData_[ch] = static_cast<char>(0);
    }
    
    // Step 4: drain numSamples from the output FIFO into buffer.
    for (int ch = 0; ch < numChannels; ++ch) {
        double* dst = buffer.getWritePointer(ch);

        if (outputFifoFill_[ch] >= need) {
            const size_t rd = outputFifoRead_[ch];
            std::copy(outputFifo_[ch].begin() + static_cast<ptrdiff_t>(rd),
                      outputFifo_[ch].begin() + static_cast<ptrdiff_t>(rd + need),
                      dst);
            outputFifoRead_[ch]  += need;
            outputFifoFill_[ch]  -= need;

            // Compact the output FIFO when the read cursor passes internalBlockSize_
            if (outputFifoRead_[ch] >= internalBlockSize_) {
                const size_t remaining = outputFifoFill_[ch];
                std::move(outputFifo_[ch].begin() +
                              static_cast<ptrdiff_t>(outputFifoRead_[ch]),
                          outputFifo_[ch].begin() +
                              static_cast<ptrdiff_t>(outputFifoRead_[ch] + remaining),
                          outputFifo_[ch].begin());
                outputFifoRead_[ch] = 0;
            }
        } else {
            // Output not yet ready (startup latency period) — emit silence
            std::fill(dst, dst + numSamples, 0.0);
        }
    }
}

// Performs analysis filter bank processing for a single block
void DTCWPTProcessor::analysisProcess(const std::vector<double>& inputBlock,
                                      AnalysisNodeGroup& nodeGroup,
                                      const std::vector<int>& nodeIds,
                                      std::vector<std::vector<double>>& results,
                                      std::vector<size_t>& cursors,
                                      std::vector<double>& workBuffer,
                                      std::vector<char>& activeFlags) {
    for (double x : inputBlock) {
        // Reset all flags at start of each sample to prevent stale state.
        // Only the root node (index 1) is initially active.
        std::fill(activeFlags.begin(), activeFlags.end(), static_cast<char>(0));
        activeFlags[1] = 1;  // Root is always active
        workBuffer[1] = x;

        // Process nodes in analysis order
        const size_t nodeCount = std::min(nodeIds.size(), nodeGroup.nodes.size());
        for (size_t i = 0; i < nodeCount; ++i) {
            int nodeId = nodeIds[i];
            
            if (!activeFlags[static_cast<size_t>(nodeId)]) {
                continue;
            }

            int leftIdx = nodeId << 1;
            int rightIdx = (nodeId << 1) | 1;

            nodeGroup.nodes[i].updateBuffer(
                workBuffer[nodeId],
                workBuffer,
                activeFlags,
                leftIdx,
                rightIdx);
        }

        // Save results for destination nodes
        for (size_t di = 0; di < destinations_.size(); ++di) {
            int destId = destinations_[di];
            if (activeFlags[static_cast<size_t>(destId)]) {
                size_t cursor = cursors[static_cast<size_t>(destId)];
                if (cursor >= results[destId].size()) {
                    throw std::runtime_error("analysis cursor overflow");
                }
                results[destId][cursor] = workBuffer[destId];
                cursors[destId] = cursor + 1;
            }
        }
    }
}

// Performs synthesis filter bank processing
void DTCWPTProcessor::synthesisProcess(std::vector<std::vector<double>>& analysedData,
                                       SynthesisNodeGroup& nodeGroup,
                                       const std::vector<int>& nodeIds,
                                       std::vector<size_t>& cursors) {
    const size_t nodeCount = std::min(nodeIds.size(), nodeGroup.nodes.size());
    for (size_t i = 0; i < nodeCount; ++i) {
        int nodeId = nodeIds[i];
        int leftChildId = nodeId << 1;
        int rightChildId = (nodeId << 1) | 1;

        // Get buffers, skip if empty
        if (analysedData[leftChildId].empty() || analysedData[rightChildId].empty()) {
            continue;
        }
        if (analysedData[nodeId].empty()) {
            continue;
        }

        std::vector<double>& lowBuf = analysedData[leftChildId];
        std::vector<double>& highBuf = analysedData[rightChildId];
        std::vector<double>& outBuf = analysedData[nodeId];

        size_t numSamples = cursors[static_cast<size_t>(leftChildId)];
        size_t writeCursor = cursors[static_cast<size_t>(nodeId)];

        nodeGroup.nodes[i].synthesizeBlock(lowBuf, highBuf, outBuf, writeCursor, numSamples);

        cursors[static_cast<size_t>(nodeId)] = writeCursor + 2 * numSamples;
    }
}

int DTCWPTProcessor::getNodeLevel(int nodeId) {
    if (nodeId <= 0) {
        return -1;
    }
    int level = 0;
    int temp = nodeId;
    while (temp > 1) {
        temp >>= 1;
        ++level;
    }
    return level;
}

int DTCWPTProcessor::getLatency() const {
    // DT-CWPT path delay + input FIFO buffering latency
    return maxDelay_ + static_cast<int>(internalBlockSize_);
}

} // namespace dtcwpt
