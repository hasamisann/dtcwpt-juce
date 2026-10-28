#include "dtcwpt_processor.h"

#include "dtcwpt_topology_planner.h"
#include "dtcwpt_filter_coeffs.h"
#include "dtcwpt_analysis_node_factory.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
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
    std::vector<double> delaySliceBuffer;

    int maxDelay = 0;

    int maxSynthesisLevel = 0;
    std::vector<std::vector<SynthesisTickPlan>> synthesisLevelPlans;
    std::vector<std::vector<double>> synthesisOutputRe;
    std::vector<std::vector<double>> synthesisOutputIm;
    std::vector<std::vector<size_t>> leafReadCursors;
    std::vector<double> silenceBuffer;
    std::vector<double> discardBuffer;

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
    std::vector<double> scDelaySliceBuffer;
    bool sidechainActive = false;
    bool sidechainProcessedThisBlock = false;
    int expectedSidechainSamples = 0;
    std::vector<std::vector<double>> scAdjustedChannels;

    BandData cachedBandData;
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

// Largest number of samples a destination at tree level `level` can receive from a host chunk of
// maxBlockSize input samples: a chunk of n consecutive input samples contains at most ceil(n / 2^level)
// multiples of 2^level, and the level-L leaf produces one sample per multiple of 2^L.
size_t calculateMaxSamplesForLevel(int maxBlockSize, int level) {
    const size_t unit = static_cast<size_t>(1) << static_cast<size_t>(level);
    return (static_cast<size_t>(maxBlockSize) + unit - 1) / unit;
}

int calculateMaxSamplesPerBand(const std::vector<int>& destinations, int maxBlockSize) {
    int minDepth = getNodeLevelFromId(destinations.front());

    for (const int dest : destinations) {
        minDepth = std::min(minDepth, getNodeLevelFromId(dest));
    }

    return static_cast<int>(calculateMaxSamplesForLevel(maxBlockSize, minDepth));
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
    , delaySliceBuffer_()
    , maxDelay_(0)
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
    , scDelaySliceBuffer_()
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

DTCWPTProcessor::WorkCounters DTCWPTProcessor::getWorkCountersForTesting() const noexcept
{
    return workCounters_;
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
    validateTopologyConfig(config);

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
        const int maxBandSamples = static_cast<int>(calculateMaxSamplesForLevel(maxBlockSize, depth));
        const int diffReSubband = (candidate.maxDelay - pathDelaysRe[i]) / rateFactor;
        const int diffImSubband = (candidate.maxDelay - pathDelaysIm[i]) / rateFactor;

        for (int ch = 0; ch < channelNum; ++ch) {
            candidate.delayBuffersRe[static_cast<size_t>(ch)].emplace_back(diffReSubband, maxBandSamples);
            candidate.delayBuffersIm[static_cast<size_t>(ch)].emplace_back(diffImSubband, maxBandSamples);
        }
    }

    const size_t maxNodeId = TopologyPlanner::MAX_SIZE;

    // A destination at level L receives at most ceil(maxBlockSize / 2^L) samples per host chunk.
    candidate.resultsRe.resize(static_cast<size_t>(channelNum));
    candidate.resultsIm.resize(static_cast<size_t>(channelNum));
    candidate.cursorsRe.resize(static_cast<size_t>(channelNum));
    candidate.cursorsIm.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.resultsRe[static_cast<size_t>(ch)].resize(maxNodeId);
        candidate.resultsIm[static_cast<size_t>(ch)].resize(maxNodeId);
        candidate.cursorsRe[static_cast<size_t>(ch)].assign(maxNodeId, 0);
        candidate.cursorsIm[static_cast<size_t>(ch)].assign(maxNodeId, 0);

        for (const int target : candidate.destinations) {
            const size_t outputSize = calculateMaxSamplesForLevel(maxBlockSize, getNodeLevel(target));

            candidate.resultsRe[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
            candidate.resultsIm[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
        }
    }

    candidate.analysisWorkBuffer.assign(maxNodeId, 0.0);
    candidate.analysisActiveFlags.assign(maxNodeId, 0);

    const size_t maxBandSamplesWidest = static_cast<size_t>(
        calculateMaxSamplesPerBand(candidate.destinations, maxBlockSize));
    candidate.delayTempBuffer.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.delayTempBuffer[static_cast<size_t>(ch)].resize(maxBandSamplesWidest);
    }
    candidate.delaySliceBuffer.resize(maxBandSamplesWidest);

    // Synthesis is scheduled tick by tick: group the synthesis nodes by level.
    std::vector<char> isDestination(maxNodeId, 0);
    for (const int dest : candidate.destinations) {
        isDestination[static_cast<size_t>(dest)] = 1;
    }
    candidate.maxSynthesisLevel = 0;
    for (const int nodeId : candidate.synthesisNodeIds) {
        candidate.maxSynthesisLevel = std::max(candidate.maxSynthesisLevel, getNodeLevel(nodeId));
    }
    candidate.synthesisLevelPlans.resize(static_cast<size_t>(candidate.maxSynthesisLevel) + 1);
    for (size_t i = 0; i < candidate.synthesisNodeIds.size(); ++i) {
        const int nodeId = candidate.synthesisNodeIds[i];
        SynthesisTickPlan tickPlan;
        tickPlan.nodeIndex = i;
        tickPlan.nodeId = nodeId;
        tickPlan.lowIsLeaf = isDestination[static_cast<size_t>(nodeId) << 1] != 0;
        tickPlan.highIsLeaf = isDestination[(static_cast<size_t>(nodeId) << 1) | 1] != 0;
        candidate.synthesisLevelPlans[static_cast<size_t>(getNodeLevel(nodeId))].push_back(tickPlan);
    }
    candidate.synthesisOutputRe.assign(static_cast<size_t>(channelNum), std::vector<double>(maxNodeId, 0.0));
    candidate.synthesisOutputIm.assign(static_cast<size_t>(channelNum), std::vector<double>(maxNodeId, 0.0));
    candidate.leafReadCursors.assign(static_cast<size_t>(channelNum), std::vector<size_t>(maxNodeId, 0));
    candidate.silenceBuffer.assign(static_cast<size_t>(maxBlockSize), 0.0);
    candidate.discardBuffer.assign(static_cast<size_t>(maxBlockSize), 0.0);

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
        const int maxBandSamples = static_cast<int>(calculateMaxSamplesForLevel(maxBlockSize, depth));
        const int diffReSubband = (candidate.maxDelay - pathDelaysRe[i]) / rateFactor;
        const int diffImSubband = (candidate.maxDelay - pathDelaysIm[i]) / rateFactor;

        for (int ch = 0; ch < channelNum; ++ch) {
            candidate.scDelayBuffersRe[static_cast<size_t>(ch)].emplace_back(diffReSubband, maxBandSamples);
            candidate.scDelayBuffersIm[static_cast<size_t>(ch)].emplace_back(diffImSubband, maxBandSamples);
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

        for (const int target : candidate.destinations) {
            const size_t outputSize = calculateMaxSamplesForLevel(maxBlockSize, getNodeLevel(target));

            candidate.scResultsRe[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
            candidate.scResultsIm[static_cast<size_t>(ch)][static_cast<size_t>(target)].assign(outputSize, 0.0);
        }
    }

    candidate.scAnalysisWorkBuffer.assign(maxNodeId, 0.0);
    candidate.scAnalysisActiveFlags.assign(maxNodeId, 0);
    candidate.scDelayTempBuffer.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.scDelayTempBuffer[static_cast<size_t>(ch)].resize(maxBandSamplesWidest);
    }
    candidate.scDelaySliceBuffer.resize(maxBandSamplesWidest);

    candidate.scAdjustedChannels.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.scAdjustedChannels[static_cast<size_t>(ch)].resize(static_cast<size_t>(maxBlockSize));
    }

    candidate.cachedBandData.bands.resize(static_cast<size_t>(channelNum));
    candidate.cachedBandData.sidechainBands.resize(static_cast<size_t>(channelNum));
    for (int ch = 0; ch < channelNum; ++ch) {
        candidate.cachedBandData.bands[static_cast<size_t>(ch)].resize(candidate.destinations.size());
        candidate.cachedBandData.sidechainBands[static_cast<size_t>(ch)].resize(candidate.destinations.size());
    }

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
    delaySliceBuffer_ = std::move(candidate.delaySliceBuffer);
    maxDelay_ = candidate.maxDelay;
    streamPos_ = 0;
    maxSynthesisLevel_ = candidate.maxSynthesisLevel;
    synthesisLevelPlans_ = std::move(candidate.synthesisLevelPlans);
    synthesisOutputRe_ = std::move(candidate.synthesisOutputRe);
    synthesisOutputIm_ = std::move(candidate.synthesisOutputIm);
    leafReadCursors_ = std::move(candidate.leafReadCursors);
    silenceBuffer_ = std::move(candidate.silenceBuffer);
    discardBuffer_ = std::move(candidate.discardBuffer);
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
    scDelaySliceBuffer_ = std::move(candidate.scDelaySliceBuffer);
    scStreamPos_ = 0;
    sidechainActive_ = candidate.sidechainActive;
    sidechainProcessedThisBlock_ = candidate.sidechainProcessedThisBlock;
    expectedSidechainSamples_ = candidate.expectedSidechainSamples;
    scAdjustedChannels_ = std::move(candidate.scAdjustedChannels);
    cachedBandData_ = std::move(candidate.cachedBandData);

    // Reset band processor if exists (lifecycle hook)
    if (bandProcessor_) {
        bandProcessor_->reset();
    }

#if DTCWPT_ENABLE_TEST_SEAMS
    workCounters_ = WorkCounters{};
#endif

    // Mark as prepared
    prepared_ = true;

    // If BandProcessor was set before prepareToPlay(), prepare it now
    if (bandProcessor_) {
        const int maxSamplesPerBand = calculateMaxSamplesPerBand(destinations_, maxBlockSize_);
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
        const int maxSamplesPerBand = calculateMaxSamplesPerBand(destinations_, maxBlockSize_);
        bandProcessor_->prepare(sampleRate_, maxSamplesPerBand,
                                static_cast<int>(destinations_.size()), channelNum_);
    }
}

void DTCWPTProcessor::processSidechain(juce::AudioBuffer<double>& sidechainBuffer) {
    const int numSamples = sidechainBuffer.getNumSamples();
    const int numChannels = sidechainBuffer.getNumChannels();

    // The adjusted copy is pre-allocated for maxBlockSize_ samples: a longer (or empty) sidechain
    // block cannot be used, so the sidechain stays off for the next processBlock().
    if (!prepared_ || numSamples <= 0 || numSamples > maxBlockSize_ || numChannels < 1) {
        sidechainProcessedThisBlock_ = false;
        return;
    }

    // Store expected sample count for validation in processBlock()
    expectedSidechainSamples_ = numSamples;

    // Channel adjustment: match main input channel count
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

// Analysis + delay compensation phase for one channel and one chunk of input samples.
void DTCWPTProcessor::processChannelAnalysisAndDelay(int ch, const double* input, size_t numSamples) {
    // Init cursors for all destinations
    for (int target : destinations_) {
        cursorsRe_[ch][static_cast<size_t>(target)] = 0;
        cursorsIm_[ch][static_cast<size_t>(target)] = 0;
    }

    // Analysis for both trees (pass per-channel results/cursors and shared work buffers)
    analysisProcess(input, numSamples, analysisNodesRe_[ch], analysisSchedulerMetadata_, resultsRe_[ch], cursorsRe_[ch],
                    analysisRuntimeStatesRe_[static_cast<size_t>(ch)], AnalysisPathId::mainReal, ch);
    analysisProcess(input, numSamples, analysisNodesIm_[ch], analysisSchedulerMetadata_, resultsIm_[ch], cursorsIm_[ch],
                    analysisRuntimeStatesIm_[static_cast<size_t>(ch)], AnalysisPathId::mainImag, ch);

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
void DTCWPTProcessor::processSidechainAnalysisAndDelay(int ch, const double* input, size_t numSamples) {
    // Init cursors for all destinations
    for (int target : destinations_) {
        scCursorsRe_[ch][static_cast<size_t>(target)] = 0;
        scCursorsIm_[ch][static_cast<size_t>(target)] = 0;
    }

    // Analysis for both trees (pass per-channel results/cursors and per-channel work buffers)
    analysisProcess(input, numSamples, scAnalysisNodesRe_[ch], analysisSchedulerMetadata_, scResultsRe_[ch], scCursorsRe_[ch],
                    scAnalysisRuntimeStatesRe_[static_cast<size_t>(ch)], AnalysisPathId::sidechainReal, ch);
    analysisProcess(input, numSamples, scAnalysisNodesIm_[ch], analysisSchedulerMetadata_, scResultsIm_[ch], scCursorsIm_[ch],
                    scAnalysisRuntimeStatesIm_[static_cast<size_t>(ch)], AnalysisPathId::sidechainImag, ch);

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

// The sidechain analysis only runs on blocks that come with a sidechain. After a gap its decimation
// phase would differ from the main stream, so the per-band sample counts would no longer match.
// Node at level l has received ceil(streamPos_ / 2^l) arrivals; its parity is that count modulo 2.
void DTCWPTProcessor::alignSidechainPhase() noexcept {
    for (auto* groups : {&scAnalysisNodesRe_, &scAnalysisNodesIm_}) {
        for (auto& group : *groups) {
            for (size_t i = 0; i < group.nodes.size(); ++i) {
                const std::uint64_t unit = std::uint64_t{1} << getNodeLevel(group.ids[i]);
                const std::uint64_t arrivals = (streamPos_ + unit - 1) / unit;
                group.nodes[i].setParity((arrivals & 1U) != 0U);
            }
        }
    }
    scStreamPos_ = streamPos_;
}

bool DTCWPTProcessor::prepareBandData(bool hasSidechain) {
    // Reuse cached BandData (pre-allocated, no resize)
    cachedBandData_.numChannels = channelNum_;
    cachedBandData_.numBands = static_cast<int>(destinations_.size());
    cachedBandData_.hasSidechain = hasSidechain;
    cachedBandData_.destinationIds = &destinations_;

    bool anySamples = false;

    // Populate bands with pointers into per-channel resultsRe_/resultsIm_
    for (int ch = 0; ch < channelNum_; ++ch) {
        for (size_t b = 0; b < destinations_.size(); ++b) {
            int dest = destinations_[b];
            size_t numSamplesInBand = cursorsRe_[ch][dest];
            cachedBandData_.bands[ch][b].re = resultsRe_[ch][dest].data();
            cachedBandData_.bands[ch][b].im = resultsIm_[ch][dest].data();
            cachedBandData_.bands[ch][b].numSamples = numSamplesInBand;
            anySamples = anySamples || numSamplesInBand > 0;
        }
    }

    // Populate sidechainBands if sidechain is active
    if (hasSidechain) {
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

    return anySamples;
}

// Synthesis + output averaging phase for one channel.
//
// Synthesis runs as a sample-by-sample schedule over the root output ticks t = streamPos_, streamPos_ + 1, ...
// The synthesis node at level k produces one output per 2^k root ticks, at the ticks with t % 2^k == 0,
// and nodes are advanced deepest level first so that a parent sees the output its child produced at the same t.
// On even node ticks (t % 2^(k+1) == 0) a node pushes the next sample of each child (the next band sample for
// a leaf, the child's latest output for an inner node); on odd node ticks it pushes zeros (upsampling).
// This is the same sequence of filter operations as synthesising a whole block per node.
void DTCWPTProcessor::processChannelSynthesisAndAverage(int ch, double* output, size_t numSamples) {
    auto& outRe = synthesisOutputRe_[static_cast<size_t>(ch)];
    auto& outIm = synthesisOutputIm_[static_cast<size_t>(ch)];
    auto& readCursor = leafReadCursors_[static_cast<size_t>(ch)];
    auto& bandsRe = resultsRe_[static_cast<size_t>(ch)];
    auto& bandsIm = resultsIm_[static_cast<size_t>(ch)];
    auto& nodesRe = synthesisNodesRe_[static_cast<size_t>(ch)].nodes;
    auto& nodesIm = synthesisNodesIm_[static_cast<size_t>(ch)].nodes;

    for (const int dest : destinations_) {
        readCursor[static_cast<size_t>(dest)] = 0;
    }

    // Next unread sample of a destination band; band samples of the current chunk are always complete
    // for the ticks of the same chunk, so running out cannot happen.
    auto takeLeaf = [&](int leafId, double& re, double& im) {
        size_t& cursor = readCursor[static_cast<size_t>(leafId)];
        assert(cursor < cursorsRe_[static_cast<size_t>(ch)][static_cast<size_t>(leafId)]);
        re = bandsRe[static_cast<size_t>(leafId)][cursor];
        im = bandsIm[static_cast<size_t>(leafId)][cursor];
        ++cursor;
    };

    for (size_t i = 0; i < numSamples; ++i) {
        const std::uint64_t tick = streamPos_ + i;
        // Levels with tick % 2^level == 0 advance on this tick (countr_zero(0) == 64)
        const int topLevel = std::min(std::countr_zero(tick), maxSynthesisLevel_);

        for (int level = topLevel; level >= 0; --level) {
            const bool pushChildren = (tick & ((std::uint64_t{2} << level) - 1)) == 0;

            for (const SynthesisTickPlan& plan : synthesisLevelPlans_[static_cast<size_t>(level)]) {
                double lowRe = 0.0, lowIm = 0.0, highRe = 0.0, highIm = 0.0;

                if (pushChildren) {
                    const int lowId = plan.nodeId << 1;
                    const int highId = lowId | 1;

                    if (plan.lowIsLeaf) {
                        takeLeaf(lowId, lowRe, lowIm);
                    } else {
                        lowRe = outRe[static_cast<size_t>(lowId)];
                        lowIm = outIm[static_cast<size_t>(lowId)];
                    }

                    if (plan.highIsLeaf) {
                        takeLeaf(highId, highRe, highIm);
                    } else {
                        highRe = outRe[static_cast<size_t>(highId)];
                        highIm = outIm[static_cast<size_t>(highId)];
                    }
                }

                outRe[static_cast<size_t>(plan.nodeId)] = nodesRe[plan.nodeIndex].processTick(lowRe, highRe);
                outIm[static_cast<size_t>(plan.nodeId)] = nodesIm[plan.nodeIndex].processTick(lowIm, highIm);

#if DTCWPT_ENABLE_TEST_SEAMS
                workCounters_.synthesisOutputSamples += 2;
#endif
            }
        }

        // Output averaging of the Re and Im trees
        output[i] = (outRe[1] + outIm[1]) / 2.0;
    }
}

// Public entry point: analyses the input samples, runs the BandProcessor on the subband samples that
// became available, and synthesises the output samples of the same time span.
void DTCWPTProcessor::processBlock(juce::AudioBuffer<double>& buffer) {
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = std::min(buffer.getNumChannels(), channelNum_);

    if (!prepared_ || numSamples == 0) return;

    // Validate sidechain sample count if sidechain was called
    // Instead of throwing (which is unsafe in audio thread), disable sidechain on mismatch
    if (sidechainProcessedThisBlock_ && numSamples != expectedSidechainSamples_) {
        // Graceful fallback: disable sidechain for this block
        sidechainActive_ = false;
        sidechainProcessedThisBlock_ = false;
    }

    const bool hasSidechain = sidechainActive_ && sidechainProcessedThisBlock_;

    // Blocks longer than maxBlockSize_ are processed in chunks that fit the pre-allocated band buffers.
    const size_t total = static_cast<size_t>(numSamples);
    const size_t chunkLimit = static_cast<size_t>(maxBlockSize_);

    for (size_t offset = 0; offset < total; offset += chunkLimit) {
        const size_t chunk = std::min(chunkLimit, total - offset);

        // Step 1: analysis (+ delay compensation) of every input sample of the chunk
        for (int ch = 0; ch < channelNum_; ++ch) {
            const double* input = ch < numChannels
                ? buffer.getReadPointer(ch) + offset
                : silenceBuffer_.data();
            processChannelAnalysisAndDelay(ch, input, chunk);
        }

        // Step 1.5: sidechain analysis, if this block comes with a sidechain
        if (hasSidechain) {
            if (scStreamPos_ != streamPos_) {
                alignSidechainPhase();
            }
            for (int ch = 0; ch < channelNum_; ++ch) {
                processSidechainAnalysisAndDelay(ch, scAdjustedChannels_[static_cast<size_t>(ch)].data() + offset, chunk);
            }
            scStreamPos_ = streamPos_ + chunk;
        }

        // Step 2: invoke the BandProcessor once with the subband samples that became available
        const bool anyBandSamples = prepareBandData(hasSidechain);
        if (bandProcessor_ && anyBandSamples) {
            bandProcessor_->processAllBands(cachedBandData_);
        }

        // Step 3: synthesis + output averaging for the same span of output ticks
        for (int ch = 0; ch < channelNum_; ++ch) {
            double* output = ch < numChannels
                ? buffer.getWritePointer(ch) + offset
                : discardBuffer_.data();
            processChannelSynthesisAndAverage(ch, output, chunk);
        }

        streamPos_ += chunk;
    }

    // Reset sidechain state for next block
    sidechainProcessedThisBlock_ = false;
}

// Performs analysis filter bank processing for a single block
void DTCWPTProcessor::analysisProcess(const double* input,
                                      size_t numSamples,
                                      AnalysisNodeGroup& nodeGroup,
                                      const AnalysisSchedulerMetadata& metadata,
                                      std::vector<std::vector<double>>& results,
                                      std::vector<size_t>& cursors,
                                      AnalysisRuntimeState& runtime,
                                      AnalysisPathId path,
                                      int channel) {
    for (int sampleIndex = 0; sampleIndex < static_cast<int>(numSamples); ++sampleIndex) {
        const double x = input[sampleIndex];
        beginAnalysisSample(runtime);
        recordArrival(runtime, 1, x);
        runtime.frontierInternalWorkItems[0] = AnalysisWorkItem{1, x};
        runtime.frontierHead = 0;
        runtime.frontierSize = 1;

        while (runtime.frontierSize > 0) {
            const AnalysisWorkItem workItem = dequeueInternalArrival(runtime);
            const int nodeId = workItem.nodeId;
            const int internalIndex = metadata.nodeIdToInternalIndex[static_cast<size_t>(nodeId)];
            if (internalIndex < 0 || static_cast<size_t>(internalIndex) >= nodeGroup.nodes.size()) {
                throw std::runtime_error("analysis internal node lookup failed");
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
                    .sampleValue = workItem.sampleValue,
                    .enteredFrontier = true,
                });
            }
#endif

            int leftIdx = nodeId << 1;
            int rightIdx = (nodeId << 1) | 1;

#if DTCWPT_ENABLE_TEST_SEAMS
            ++workCounters_.analysisArrivals;
#endif

            AnalysisChildOutputs outputs{};
            const bool emitted = nodeGroup.nodes[static_cast<size_t>(internalIndex)].processArrival(
                workItem.sampleValue,
                outputs);

            if (!emitted) {
                continue;
            }

            recordArrival(runtime, leftIdx, outputs.low);
            recordArrival(runtime, rightIdx, outputs.high);

            const bool leftIsInternal = leftIdx > 0
                && leftIdx < static_cast<int>(metadata.nodeIdToInternalIndex.size())
                && metadata.nodeIdToInternalIndex[static_cast<size_t>(leftIdx)] >= 0;
            const bool rightIsInternal = rightIdx > 0
                && rightIdx < static_cast<int>(metadata.nodeIdToInternalIndex.size())
                && metadata.nodeIdToInternalIndex[static_cast<size_t>(rightIdx)] >= 0;

#if DTCWPT_ENABLE_TEST_SEAMS
            if (analysisTraceSink_ != nullptr) {
                analysisTraceSink_->record(AnalysisTraceEvent{
                    .kind = AnalysisTraceEventKind::childArrivalRecorded,
                    .path = path,
                    .channel = channel,
                    .sampleIndex = sampleIndex,
                    .nodeId = nodeId,
                    .relatedNodeId = leftIdx,
                    .cursorBeforeWrite = 0,
                    .sampleValue = outputs.low,
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
                    .sampleValue = outputs.high,
                    .enteredFrontier = rightIsInternal,
                });
            }
#endif

            appendProcessedParentInternalChildren(
                runtime,
                nodeId,
                leftIsInternal ? std::optional<AnalysisWorkItem>(AnalysisWorkItem{leftIdx, outputs.low}) : std::nullopt,
                rightIsInternal ? std::optional<AnalysisWorkItem>(AnalysisWorkItem{rightIdx, outputs.high}) : std::nullopt);
        }

        // Save results for destination nodes
        for (const int destId : metadata.destinationIdsInOrder) {
            if (!hasCurrentSampleArrival(runtime, destId)) {
                continue;
            }

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
                    .sampleValue = runtime.transportedValues[static_cast<size_t>(destId)],
                    .enteredFrontier = false,
                });
            }
#endif

            results[destId][cursor] = runtime.transportedValues[static_cast<size_t>(destId)];
            cursors[destId] = cursor + 1;
        }
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
    // The engine is streaming: the only latency is the DT-CWPT path delay.
    return maxDelay_;
}

} // namespace dtcwpt
