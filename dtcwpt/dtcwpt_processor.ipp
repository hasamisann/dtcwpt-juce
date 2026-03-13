#include "dtcwpt_processor.h"

#include "dtcwpt_topology_planner.h"
#include "dtcwpt_filter_coeffs.h"
#include "dtcwpt_analysis_node_factory.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

namespace dtcwpt {

namespace {

struct TopologyValidationResult {
    int actualDepth = 0;
    int configuredMaxDepth = 0;
    int supportedMaxDepth = topology_limits::kSupportedMaxDepth;
    int maxNodeIndex = 0;
};

struct PathShapeState {
    bool isLeaf = false;
    bool hasLowChild = false;
    bool hasHighChild = false;
};

struct PreparedProcessorState {
    std::unique_ptr<TopologyPlanner> topology;
    std::vector<int> analysisOrder;
    std::vector<int> synthesisOrder;
    std::vector<int> destinations;

    std::vector<AnalysisNodeGroup> analysisNodesRe;
    std::vector<AnalysisNodeGroup> analysisNodesIm;
    std::vector<SynthesisNodeGroup> synthesisNodesRe;
    std::vector<SynthesisNodeGroup> synthesisNodesIm;
    std::vector<int> analysisNodeIds;
    AnalysisSchedulerMetadata analysisSchedulerMetadata;
    std::vector<AnalysisRuntimeState> analysisRuntimeStatesRe;
    std::vector<AnalysisRuntimeState> analysisRuntimeStatesIm;
    std::vector<int> synthesisNodeIds;
    std::vector<std::vector<DelayBuffer>> delayBuffersRe;
    std::vector<std::vector<DelayBuffer>> delayBuffersIm;

    std::vector<double> analysisWorkBuffer;
    std::vector<char> analysisActiveFlags;
    std::vector<std::vector<std::vector<double>>> resultsRe;
    std::vector<std::vector<std::vector<double>>> resultsIm;
    std::vector<std::vector<size_t>> cursorsRe;
    std::vector<std::vector<size_t>> cursorsIm;
    std::vector<std::vector<double>> delayTempBuffer;
    std::vector<double> inputBlockBuffer;
    std::vector<double> delaySliceBuffer;

    int maxDelay = 0;
    size_t alignUnit = 1;
    size_t internalBlockSize = 0;
    std::vector<std::vector<double>> inputFifo;
    std::vector<size_t> inputFifoFill;
    std::vector<std::vector<double>> outputFifo;
    std::vector<size_t> outputFifoFill;
    std::vector<size_t> outputFifoRead;

    std::vector<AnalysisNodeGroup> scAnalysisNodesRe;
    std::vector<AnalysisNodeGroup> scAnalysisNodesIm;
    std::vector<AnalysisRuntimeState> scAnalysisRuntimeStatesRe;
    std::vector<AnalysisRuntimeState> scAnalysisRuntimeStatesIm;
    std::vector<std::vector<DelayBuffer>> scDelayBuffersRe;
    std::vector<std::vector<DelayBuffer>> scDelayBuffersIm;
    std::vector<std::vector<std::vector<double>>> scResultsRe;
    std::vector<std::vector<std::vector<double>>> scResultsIm;
    std::vector<std::vector<size_t>> scCursorsRe;
    std::vector<std::vector<size_t>> scCursorsIm;
    std::vector<double> scAnalysisWorkBuffer;
    std::vector<char> scAnalysisActiveFlags;
    std::vector<std::vector<double>> scDelayTempBuffer;
    std::vector<double> scInputBlockBuffer;
    std::vector<double> scDelaySliceBuffer;
    std::vector<std::vector<double>> scInputFifo;
    std::vector<size_t> scInputFifoFill;
    bool sidechainActive = false;
    bool sidechainProcessedThisBlock = false;
    int expectedSidechainSamples = 0;
    std::vector<std::vector<double>> scAdjustedChannels;

    std::vector<std::vector<double>> alignedInputBuffers;
    std::vector<std::vector<double>> alignedSidechainBuffers;
    juce::AudioBuffer<double> alignedOutputBuffer;
    BandData cachedBandData;
    std::vector<char> channelHasData;
    std::vector<char> scChannelHasData;
};

int getNodeLevelFromId(int nodeId) {
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

int pathToNodeIndex(const std::string& path) noexcept {
    int index = 1;
    for (const char c : path) {
        index = (index << 1) | (c == 'H' ? 1 : 0);
    }
    return index;
}

void validatePathSyntax(const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("Destination paths must be non-empty");
    }

    for (const char c : path) {
        if (c != 'L' && c != 'H') {
            throw std::invalid_argument("Destination paths may contain only 'L' or 'H'");
        }
    }
}

std::map<std::string, PathShapeState> buildPathShapeStates(const std::set<std::string>& destinations) {
    std::map<std::string, PathShapeState> states;

    for (const auto& destination : destinations) {
        states[destination].isLeaf = true;

        std::string prefix;
        prefix.reserve(destination.size());
        for (const char c : destination) {
            auto& state = states[prefix];
            if (c == 'L') {
                state.hasLowChild = true;
            } else {
                state.hasHighChild = true;
            }
            prefix.push_back(c);
        }
    }

    return states;
}

void validateNoAncestorOverlap(const std::set<std::string>& destinations) {
    for (const auto& destination : destinations) {
        for (std::size_t prefixLength = 1; prefixLength < destination.size(); ++prefixLength) {
            if (destinations.contains(destination.substr(0, prefixLength))) {
                throw std::invalid_argument("Destination paths must not overlap ancestor leaves");
            }
        }
    }
}

void validateCompleteLeafSet(const std::map<std::string, PathShapeState>& states) {
    for (const auto& [path, state] : states) {
        const bool hasChildren = state.hasLowChild || state.hasHighChild;

        if (state.isLeaf && hasChildren) {
            throw std::invalid_argument("Destination paths must not be both leaves and internal nodes");
        }

        if (!state.isLeaf && (!state.hasLowChild || !state.hasHighChild)) {
            throw std::invalid_argument("Topology must form a complete binary leaf set");
        }

        (void) path;
    }
}

TopologyValidationResult validateTopologyConfig(const TopologyConfig& config) {
    if (config.destinations.empty()) {
        throw std::invalid_argument("At least one destination required");
    }

    if (config.maxDepth < 1 || config.maxDepth > topology_limits::kSupportedMaxDepth) {
        throw std::invalid_argument("maxDepth must be within supported bounds");
    }

    TopologyValidationResult result;
    result.configuredMaxDepth = config.maxDepth;

    std::set<std::string> uniqueDestinations;

    for (const auto& path : config.destinations) {
        validatePathSyntax(path);

        if (!uniqueDestinations.insert(path).second) {
            throw std::invalid_argument("Destination paths must be unique");
        }

        result.actualDepth = std::max(result.actualDepth, static_cast<int>(path.size()));

        const int nodeIndex = pathToNodeIndex(path);
        result.maxNodeIndex = std::max(result.maxNodeIndex, nodeIndex);
    }

    validateNoAncestorOverlap(uniqueDestinations);
    validateCompleteLeafSet(buildPathShapeStates(uniqueDestinations));

    if (result.actualDepth > result.configuredMaxDepth) {
        throw std::invalid_argument("Topology depth exceeds configured maxDepth");
    }

    if (result.actualDepth > result.supportedMaxDepth) {
        throw std::invalid_argument("Topology depth exceeds supported limit");
    }

    if (result.maxNodeIndex < 0 ||
        static_cast<std::size_t>(result.maxNodeIndex) >= topology_limits::kPlannerArraySize) {
        throw std::invalid_argument("Destination path exceeds supported topology depth");
    }

    return result;
}

int calculateMaxSamplesPerBand(const std::vector<int>& destinations, size_t internalBlockSize) {
    size_t minDepth = static_cast<size_t>(getNodeLevelFromId(destinations.front()));

    for (const int dest : destinations) {
        const size_t depth = static_cast<size_t>(getNodeLevelFromId(dest));
        minDepth = std::min(minDepth, depth);
    }

    int maxSamplesPerBand = static_cast<int>(internalBlockSize >> minDepth);
    if (maxSamplesPerBand < 1) {
        maxSamplesPerBand = 1;
    }

    return maxSamplesPerBand;
}

} // namespace

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
    , analysisSchedulerMetadata_()
    , analysisRuntimeStatesRe_()
    , analysisRuntimeStatesIm_()
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
    , scAnalysisRuntimeStatesRe_()
    , scAnalysisRuntimeStatesIm_()
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
#if DTCWPT_ENABLE_TEST_SEAMS
    , prepareCandidateFailpoint_(PrepareCandidateFailpoint::none)
    , analysisTraceSink_(nullptr)
#endif
    , expectedSidechainSamples_(0) {
}

#if DTCWPT_ENABLE_TEST_SEAMS
void DTCWPTProcessor::setPrepareCandidateFailpointForTesting(PrepareCandidateFailpoint failpoint) noexcept
{
    prepareCandidateFailpoint_ = failpoint;
}

void DTCWPTProcessor::setAnalysisTraceSinkForTesting(AnalysisTraceSink* sink) noexcept
{
    analysisTraceSink_ = sink;
}
#endif

void DTCWPTProcessor::prepareToPlay(double sampleRate, int maxBlockSize,
                                    const TopologyConfig& config, int channelNum) {
    if (sampleRate <= 0.0) {
        throw std::invalid_argument("Sample rate must be positive");
    }
    if (maxBlockSize <= 0) {
        throw std::invalid_argument("Block size must be positive");
    }
    if (channelNum < 1 || channelNum > 2) {
        throw std::invalid_argument("Channel count must be 1 or 2");
    }
    const TopologyValidationResult validation = validateTopologyConfig(config);

    PreparedProcessorState candidate;
    candidate.topology = std::make_unique<TopologyPlanner>(config.destinations);
    auto plan = candidate.topology->getPlan();
    candidate.analysisOrder = std::move(plan.first);
    candidate.synthesisOrder = std::move(plan.second);
    candidate.destinations = candidate.topology->destinations;

    candidate.analysisNodesRe.resize(static_cast<size_t>(channelNum));
    candidate.analysisNodesIm.resize(static_cast<size_t>(channelNum));
    candidate.synthesisNodesRe.resize(static_cast<size_t>(channelNum));
    candidate.synthesisNodesIm.resize(static_cast<size_t>(channelNum));
    candidate.delayBuffersRe.resize(static_cast<size_t>(channelNum));
    candidate.delayBuffersIm.resize(static_cast<size_t>(channelNum));

    for (int ch = 0; ch < channelNum; ++ch) {
        AnalysisNode nodeRe(createAnalysisNodeForId(1, AnalysisTreeKind::real));
        AnalysisNode nodeIm(createAnalysisNodeForId(1, AnalysisTreeKind::imag));
        candidate.analysisNodesRe[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeRe));
        candidate.analysisNodesIm[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeIm));
    }
    candidate.analysisNodeIds.push_back(1);

    for (const int idx : candidate.analysisOrder) {
        if (idx == 1) {
            continue;
        }

        candidate.analysisNodeIds.push_back(idx);

        for (int ch = 0; ch < channelNum; ++ch) {
            AnalysisNode nodeRe(createAnalysisNodeForId(idx, AnalysisTreeKind::real));
            AnalysisNode nodeIm(createAnalysisNodeForId(idx, AnalysisTreeKind::imag));
            candidate.analysisNodesRe[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeRe));
            candidate.analysisNodesIm[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeIm));
        }
    }

    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.analysisNodesRe[static_cast<size_t>(ch)].ids = candidate.analysisNodeIds;
        candidate.analysisNodesIm[static_cast<size_t>(ch)].ids = candidate.analysisNodeIds;
    }

    candidate.analysisSchedulerMetadata = buildAnalysisSchedulerMetadata(
        candidate.analysisNodeIds,
        candidate.destinations,
        static_cast<int>(TopologyPlanner::MAX_SIZE));
    candidate.analysisRuntimeStatesRe.resize(static_cast<size_t>(channelNum));
    candidate.analysisRuntimeStatesIm.resize(static_cast<size_t>(channelNum));
    candidate.scAnalysisRuntimeStatesRe.resize(static_cast<size_t>(channelNum));
    candidate.scAnalysisRuntimeStatesIm.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.analysisRuntimeStatesRe[static_cast<size_t>(ch)] =
            buildAnalysisRuntimeState(candidate.analysisSchedulerMetadata);
        candidate.analysisRuntimeStatesIm[static_cast<size_t>(ch)] =
            buildAnalysisRuntimeState(candidate.analysisSchedulerMetadata);
        candidate.scAnalysisRuntimeStatesRe[static_cast<size_t>(ch)] =
            buildAnalysisRuntimeState(candidate.analysisSchedulerMetadata);
        candidate.scAnalysisRuntimeStatesIm[static_cast<size_t>(ch)] =
            buildAnalysisRuntimeState(candidate.analysisSchedulerMetadata);
    }

    for (const int idx : candidate.synthesisOrder) {
        candidate.synthesisNodeIds.push_back(idx);

        for (int ch = 0; ch < channelNum; ++ch) {
            if (idx == 1) {
                SynthesisNode nodeRe(filters::CDF_RE, true);
                SynthesisNode nodeIm(filters::CDF_IM, true);
                candidate.synthesisNodesRe[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeRe));
                candidate.synthesisNodesIm[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeIm));
            } else if (idx % 2 == 1) {
                SynthesisNode nodeRe(filters::PACKET, false);
                SynthesisNode nodeIm(filters::PACKET, false);
                candidate.synthesisNodesRe[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeRe));
                candidate.synthesisNodesIm[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeIm));
            } else {
                SynthesisNode nodeRe(filters::QSHIFT14_RE, false);
                SynthesisNode nodeIm(filters::QSHIFT14_IM, false);
                candidate.synthesisNodesRe[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeRe));
                candidate.synthesisNodesIm[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeIm));
            }
        }
    }

    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.synthesisNodesRe[static_cast<size_t>(ch)].ids = candidate.synthesisNodeIds;
        candidate.synthesisNodesIm[static_cast<size_t>(ch)].ids = candidate.synthesisNodeIds;
    }

    std::vector<int> pathDelaysRe(candidate.destinations.size());
    std::vector<int> pathDelaysIm(candidate.destinations.size());
    for (size_t i = 0; i < candidate.destinations.size(); ++i) {
        const int dest = candidate.destinations[i];
        const int delayRe = calculateDelayForNode(dest, candidate.synthesisNodesRe[0]);
        const int delayIm = calculateDelayForNode(dest, candidate.synthesisNodesIm[0]);
        pathDelaysRe[i] = delayRe;
        pathDelaysIm[i] = delayIm;
        candidate.maxDelay = std::max(candidate.maxDelay, std::max(delayRe, delayIm));
    }

    for (size_t i = 0; i < candidate.destinations.size(); ++i) {
        const int dest = candidate.destinations[i];
        const int depth = getNodeLevel(dest);
        const int rateFactor = 1 << depth;
        const int diffReSubband = (candidate.maxDelay - pathDelaysRe[i]) / rateFactor;
        const int diffImSubband = (candidate.maxDelay - pathDelaysIm[i]) / rateFactor;

        for (int ch = 0; ch < channelNum; ++ch) {
            candidate.delayBuffersRe[static_cast<size_t>(ch)].emplace_back(diffReSubband, maxBlockSize);
            candidate.delayBuffersIm[static_cast<size_t>(ch)].emplace_back(diffImSubband, maxBlockSize);
        }
    }

    candidate.alignUnit = static_cast<size_t>(1) << static_cast<size_t>(validation.actualDepth);
    const size_t maxBlockSizeValue = static_cast<size_t>(maxBlockSize);
    candidate.internalBlockSize = ((maxBlockSizeValue + candidate.alignUnit - 1) / candidate.alignUnit)
        * candidate.alignUnit;

    const size_t maxNodeId = TopologyPlanner::MAX_SIZE;

    candidate.resultsRe.resize(static_cast<size_t>(channelNum));
    candidate.resultsIm.resize(static_cast<size_t>(channelNum));
    candidate.cursorsRe.resize(static_cast<size_t>(channelNum));
    candidate.cursorsIm.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.resultsRe[static_cast<size_t>(ch)].resize(maxNodeId);
        candidate.resultsIm[static_cast<size_t>(ch)].resize(maxNodeId);
        candidate.cursorsRe[static_cast<size_t>(ch)].assign(maxNodeId, 0);
        candidate.cursorsIm[static_cast<size_t>(ch)].assign(maxNodeId, 0);

        for (const int target : candidate.analysisOrder) {
            const int depth = getNodeLevel(target);
            size_t outputSize = candidate.internalBlockSize >> static_cast<size_t>(depth);
            if (outputSize < 1) {
                outputSize = 1;
            }

            candidate.resultsRe[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
            candidate.resultsIm[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
        }

        for (const int target : candidate.destinations) {
            const int depth = getNodeLevel(target);
            size_t outputSize = candidate.internalBlockSize >> static_cast<size_t>(depth);
            if (outputSize < 1) {
                outputSize = 1;
            }

            candidate.resultsRe[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
            candidate.resultsIm[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
        }
    }

    candidate.analysisWorkBuffer.assign(maxNodeId, 0.0);
    candidate.analysisActiveFlags.assign(maxNodeId, 0);
    candidate.delayTempBuffer.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.delayTempBuffer[static_cast<size_t>(ch)].resize(candidate.internalBlockSize);
    }
    candidate.inputBlockBuffer.resize(candidate.internalBlockSize);
    candidate.delaySliceBuffer.resize(candidate.internalBlockSize);

    const size_t inputFifoSize = candidate.internalBlockSize * 2;
    const size_t outputFifoSize = static_cast<size_t>(candidate.maxDelay) + candidate.internalBlockSize * 4;
    candidate.inputFifo.assign(static_cast<size_t>(channelNum), std::vector<double>(inputFifoSize, 0.0));
    candidate.outputFifo.assign(static_cast<size_t>(channelNum), std::vector<double>(outputFifoSize, 0.0));
    candidate.inputFifoFill.assign(static_cast<size_t>(channelNum), 0);
    candidate.outputFifoFill.assign(static_cast<size_t>(channelNum), 0);
    candidate.outputFifoRead.assign(static_cast<size_t>(channelNum), 0);
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.outputFifoFill[static_cast<size_t>(ch)] = candidate.internalBlockSize;
    }

    candidate.scAnalysisNodesRe.resize(static_cast<size_t>(channelNum));
    candidate.scAnalysisNodesIm.resize(static_cast<size_t>(channelNum));
    candidate.scDelayBuffersRe.resize(static_cast<size_t>(channelNum));
    candidate.scDelayBuffersIm.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        AnalysisNode nodeRe(createAnalysisNodeForId(1, AnalysisTreeKind::real));
        AnalysisNode nodeIm(createAnalysisNodeForId(1, AnalysisTreeKind::imag));
        candidate.scAnalysisNodesRe[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeRe));
        candidate.scAnalysisNodesIm[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeIm));
    }

    for (const int idx : candidate.analysisOrder) {
        if (idx == 1) {
            continue;
        }

        for (int ch = 0; ch < channelNum; ++ch) {
            AnalysisNode nodeRe(createAnalysisNodeForId(idx, AnalysisTreeKind::real));
            AnalysisNode nodeIm(createAnalysisNodeForId(idx, AnalysisTreeKind::imag));
            candidate.scAnalysisNodesRe[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeRe));
            candidate.scAnalysisNodesIm[static_cast<size_t>(ch)].nodes.push_back(std::move(nodeIm));
        }
    }

    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.scAnalysisNodesRe[static_cast<size_t>(ch)].ids = candidate.analysisNodeIds;
        candidate.scAnalysisNodesIm[static_cast<size_t>(ch)].ids = candidate.analysisNodeIds;
    }

    for (size_t i = 0; i < candidate.destinations.size(); ++i) {
        const int dest = candidate.destinations[i];
        const int depth = getNodeLevel(dest);
        const int rateFactor = 1 << depth;
        const int diffReSubband = (candidate.maxDelay - pathDelaysRe[i]) / rateFactor;
        const int diffImSubband = (candidate.maxDelay - pathDelaysIm[i]) / rateFactor;

        for (int ch = 0; ch < channelNum; ++ch) {
            candidate.scDelayBuffersRe[static_cast<size_t>(ch)].emplace_back(diffReSubband, maxBlockSize);
            candidate.scDelayBuffersIm[static_cast<size_t>(ch)].emplace_back(diffImSubband, maxBlockSize);
        }
    }

    candidate.scResultsRe.resize(static_cast<size_t>(channelNum));
    candidate.scResultsIm.resize(static_cast<size_t>(channelNum));
    candidate.scCursorsRe.resize(static_cast<size_t>(channelNum));
    candidate.scCursorsIm.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.scResultsRe[static_cast<size_t>(ch)].resize(maxNodeId);
        candidate.scResultsIm[static_cast<size_t>(ch)].resize(maxNodeId);
        candidate.scCursorsRe[static_cast<size_t>(ch)].assign(maxNodeId, 0);
        candidate.scCursorsIm[static_cast<size_t>(ch)].assign(maxNodeId, 0);

        for (const int target : candidate.analysisOrder) {
            const int depth = getNodeLevel(target);
            size_t outputSize = candidate.internalBlockSize >> static_cast<size_t>(depth);
            if (outputSize < 1) {
                outputSize = 1;
            }

            candidate.scResultsRe[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
            candidate.scResultsIm[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
        }

        for (const int target : candidate.destinations) {
            const int depth = getNodeLevel(target);
            size_t outputSize = candidate.internalBlockSize >> static_cast<size_t>(depth);
            if (outputSize < 1) {
                outputSize = 1;
            }

            candidate.scResultsRe[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
            candidate.scResultsIm[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
        }
    }

    candidate.scAnalysisWorkBuffer.assign(maxNodeId, 0.0);
    candidate.scAnalysisActiveFlags.assign(maxNodeId, 0);
    candidate.scDelayTempBuffer.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.scDelayTempBuffer[static_cast<size_t>(ch)].resize(candidate.internalBlockSize);
    }
    candidate.scInputBlockBuffer.resize(candidate.internalBlockSize);
    candidate.scDelaySliceBuffer.resize(candidate.internalBlockSize);
    candidate.scInputFifo.assign(static_cast<size_t>(channelNum),
                                 std::vector<double>(candidate.internalBlockSize * 2, 0.0));
    candidate.scInputFifoFill.assign(static_cast<size_t>(channelNum), 0);

    candidate.scAdjustedChannels.resize(static_cast<size_t>(channelNum));
    candidate.alignedInputBuffers.resize(static_cast<size_t>(channelNum));
    candidate.alignedSidechainBuffers.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.scAdjustedChannels[static_cast<size_t>(ch)].resize(candidate.internalBlockSize);
        candidate.alignedInputBuffers[static_cast<size_t>(ch)].resize(candidate.internalBlockSize);
        candidate.alignedSidechainBuffers[static_cast<size_t>(ch)].resize(candidate.internalBlockSize);
    }
    candidate.alignedOutputBuffer.setSize(1, static_cast<int>(candidate.internalBlockSize));

    candidate.cachedBandData.bands.resize(static_cast<size_t>(channelNum));
    candidate.cachedBandData.sidechainBands.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.cachedBandData.bands[static_cast<size_t>(ch)].resize(candidate.destinations.size());
        candidate.cachedBandData.sidechainBands[static_cast<size_t>(ch)].resize(candidate.destinations.size());
    }

    candidate.channelHasData.resize(static_cast<size_t>(channelNum));
    candidate.scChannelHasData.resize(static_cast<size_t>(channelNum));

#if DTCWPT_ENABLE_TEST_SEAMS
    if (prepareCandidateFailpoint_ == PrepareCandidateFailpoint::beforeCommit) {
        prepareCandidateFailpoint_ = PrepareCandidateFailpoint::none;
        throw std::runtime_error("prepare candidate failpoint");
    }
#endif

    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    channelNum_ = channelNum;
    topology_ = std::move(candidate.topology);
    analysisOrder_ = std::move(candidate.analysisOrder);
    synthesisOrder_ = std::move(candidate.synthesisOrder);
    destinations_ = std::move(candidate.destinations);
    analysisNodesRe_ = std::move(candidate.analysisNodesRe);
    analysisNodesIm_ = std::move(candidate.analysisNodesIm);
    synthesisNodesRe_ = std::move(candidate.synthesisNodesRe);
    synthesisNodesIm_ = std::move(candidate.synthesisNodesIm);
    analysisNodeIds_ = std::move(candidate.analysisNodeIds);
    analysisSchedulerMetadata_ = std::move(candidate.analysisSchedulerMetadata);
    analysisRuntimeStatesRe_ = std::move(candidate.analysisRuntimeStatesRe);
    analysisRuntimeStatesIm_ = std::move(candidate.analysisRuntimeStatesIm);
    synthesisNodeIds_ = std::move(candidate.synthesisNodeIds);
    delayBuffersRe_ = std::move(candidate.delayBuffersRe);
    delayBuffersIm_ = std::move(candidate.delayBuffersIm);
    analysisWorkBuffer_ = std::move(candidate.analysisWorkBuffer);
    analysisActiveFlags_ = std::move(candidate.analysisActiveFlags);
    resultsRe_ = std::move(candidate.resultsRe);
    resultsIm_ = std::move(candidate.resultsIm);
    cursorsRe_ = std::move(candidate.cursorsRe);
    cursorsIm_ = std::move(candidate.cursorsIm);
    delayTempBuffer_ = std::move(candidate.delayTempBuffer);
    inputBlockBuffer_ = std::move(candidate.inputBlockBuffer);
    delaySliceBuffer_ = std::move(candidate.delaySliceBuffer);
    maxDelay_ = candidate.maxDelay;
    alignUnit_ = candidate.alignUnit;
    internalBlockSize_ = candidate.internalBlockSize;
    inputFifo_ = std::move(candidate.inputFifo);
    inputFifoFill_ = std::move(candidate.inputFifoFill);
    outputFifo_ = std::move(candidate.outputFifo);
    outputFifoFill_ = std::move(candidate.outputFifoFill);
    outputFifoRead_ = std::move(candidate.outputFifoRead);
    scAnalysisNodesRe_ = std::move(candidate.scAnalysisNodesRe);
    scAnalysisNodesIm_ = std::move(candidate.scAnalysisNodesIm);
    scAnalysisRuntimeStatesRe_ = std::move(candidate.scAnalysisRuntimeStatesRe);
    scAnalysisRuntimeStatesIm_ = std::move(candidate.scAnalysisRuntimeStatesIm);
    scDelayBuffersRe_ = std::move(candidate.scDelayBuffersRe);
    scDelayBuffersIm_ = std::move(candidate.scDelayBuffersIm);
    scResultsRe_ = std::move(candidate.scResultsRe);
    scResultsIm_ = std::move(candidate.scResultsIm);
    scCursorsRe_ = std::move(candidate.scCursorsRe);
    scCursorsIm_ = std::move(candidate.scCursorsIm);
    scAnalysisWorkBuffer_ = std::move(candidate.scAnalysisWorkBuffer);
    scAnalysisActiveFlags_ = std::move(candidate.scAnalysisActiveFlags);
    scDelayTempBuffer_ = std::move(candidate.scDelayTempBuffer);
    scInputBlockBuffer_ = std::move(candidate.scInputBlockBuffer);
    scDelaySliceBuffer_ = std::move(candidate.scDelaySliceBuffer);
    scInputFifo_ = std::move(candidate.scInputFifo);
    scInputFifoFill_ = std::move(candidate.scInputFifoFill);
    sidechainActive_ = candidate.sidechainActive;
    sidechainProcessedThisBlock_ = candidate.sidechainProcessedThisBlock;
    expectedSidechainSamples_ = candidate.expectedSidechainSamples;
    scAdjustedChannels_ = std::move(candidate.scAdjustedChannels);
    alignedInputBuffers_ = std::move(candidate.alignedInputBuffers);
    alignedSidechainBuffers_ = std::move(candidate.alignedSidechainBuffers);
    alignedOutputBuffer_ = std::move(candidate.alignedOutputBuffer);
    cachedBandData_ = std::move(candidate.cachedBandData);
    channelHasData_ = std::move(candidate.channelHasData);
    scChannelHasData_ = std::move(candidate.scChannelHasData);

    // Reset band processor if exists (lifecycle hook)
    if (bandProcessor_) {
        bandProcessor_->reset();
    }

    // Mark as prepared
    prepared_ = true;

    // If BandProcessor was set before prepareToPlay(), prepare it now
    if (bandProcessor_) {
        const int maxSamplesPerBand = calculateMaxSamplesPerBand(destinations_, internalBlockSize_);
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
        const int maxSamplesPerBand = calculateMaxSamplesPerBand(destinations_, internalBlockSize_);
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
                    analysisWorkBuffer_, analysisActiveFlags_, AnalysisPathId::mainReal, ch);
    analysisProcess(inputBlock, analysisNodesIm_[ch], analysisNodeIds_, resultsIm_[ch], cursorsIm_[ch],
                    analysisWorkBuffer_, analysisActiveFlags_, AnalysisPathId::mainImag, ch);

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
                    scAnalysisWorkBuffer_, scAnalysisActiveFlags_, AnalysisPathId::sidechainReal, ch);
    analysisProcess(inputBlock, scAnalysisNodesIm_[ch], analysisNodeIds_, scResultsIm_[ch], scCursorsIm_[ch],
                    scAnalysisWorkBuffer_, scAnalysisActiveFlags_, AnalysisPathId::sidechainImag, ch);

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
                                      std::vector<char>& activeFlags,
                                      AnalysisPathId path,
                                      int channel) {
    for (int sampleIndex = 0; sampleIndex < static_cast<int>(inputBlock.size()); ++sampleIndex) {
        const double x = inputBlock[static_cast<size_t>(sampleIndex)];
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

#if DTCWPT_ENABLE_TEST_SEAMS
            if (analysisTraceSink_ != nullptr) {
                analysisTraceSink_->record(AnalysisTraceEvent{
                    .kind = AnalysisTraceEventKind::dequeue,
                    .path = path,
                    .channel = channel,
                    .sampleIndex = sampleIndex,
                    .nodeId = nodeId,
                    .relatedNodeId = 0,
                    .cursorBeforeWrite = 0,
                    .sampleValue = workBuffer[static_cast<size_t>(nodeId)],
                    .enteredFrontier = true,
                });
            }
#endif

            int leftIdx = nodeId << 1;
            int rightIdx = (nodeId << 1) | 1;

            const bool emitted = nodeGroup.nodes[i].updateBuffer(
                workBuffer[nodeId],
                workBuffer,
                activeFlags,
                leftIdx,
                rightIdx);

#if DTCWPT_ENABLE_TEST_SEAMS
            if (emitted && analysisTraceSink_ != nullptr) {
                const bool leftIsInternal = leftIdx > 0
                    && leftIdx < static_cast<int>(analysisSchedulerMetadata_.nodeIdToInternalIndex.size())
                    && analysisSchedulerMetadata_.nodeIdToInternalIndex[static_cast<size_t>(leftIdx)] >= 0;
                const bool rightIsInternal = rightIdx > 0
                    && rightIdx < static_cast<int>(analysisSchedulerMetadata_.nodeIdToInternalIndex.size())
                    && analysisSchedulerMetadata_.nodeIdToInternalIndex[static_cast<size_t>(rightIdx)] >= 0;

                analysisTraceSink_->record(AnalysisTraceEvent{
                    .kind = AnalysisTraceEventKind::childArrivalRecorded,
                    .path = path,
                    .channel = channel,
                    .sampleIndex = sampleIndex,
                    .nodeId = nodeId,
                    .relatedNodeId = leftIdx,
                    .cursorBeforeWrite = 0,
                    .sampleValue = workBuffer[static_cast<size_t>(leftIdx)],
                    .enteredFrontier = leftIsInternal,
                });
                analysisTraceSink_->record(AnalysisTraceEvent{
                    .kind = AnalysisTraceEventKind::childArrivalRecorded,
                    .path = path,
                    .channel = channel,
                    .sampleIndex = sampleIndex,
                    .nodeId = nodeId,
                    .relatedNodeId = rightIdx,
                    .cursorBeforeWrite = 0,
                    .sampleValue = workBuffer[static_cast<size_t>(rightIdx)],
                    .enteredFrontier = rightIsInternal,
                });
            }
#endif
        }

        // Save results for destination nodes
        for (size_t di = 0; di < destinations_.size(); ++di) {
            int destId = destinations_[di];
            if (activeFlags[static_cast<size_t>(destId)]) {
                size_t cursor = cursors[static_cast<size_t>(destId)];
                if (cursor >= results[destId].size()) {
                    throw std::runtime_error("analysis cursor overflow");
                }

#if DTCWPT_ENABLE_TEST_SEAMS
                if (analysisTraceSink_ != nullptr) {
                    analysisTraceSink_->record(AnalysisTraceEvent{
                        .kind = AnalysisTraceEventKind::destinationWrite,
                        .path = path,
                        .channel = channel,
                        .sampleIndex = sampleIndex,
                        .nodeId = destId,
                        .relatedNodeId = 0,
                        .cursorBeforeWrite = cursor,
                        .sampleValue = workBuffer[static_cast<size_t>(destId)],
                        .enteredFrontier = false,
                    });
                }
#endif

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
