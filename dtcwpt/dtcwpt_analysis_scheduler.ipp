#include "dtcwpt_analysis_scheduler.h"

#include <stdexcept>

namespace dtcwpt {

namespace {

std::size_t ringIndex(const AnalysisRuntimeState& state, std::size_t logicalOffset) noexcept
{
    return (state.frontierHead + logicalOffset) % state.frontierInternalWorkItems.size();
}

bool isExpectedChildOfParent(int parentNodeId, int childNodeId, bool isLowChild) noexcept
{
    const int expectedChild = isLowChild ? (parentNodeId * 2) : (parentNodeId * 2 + 1);
    return childNodeId == expectedChild;
}

} // namespace

AnalysisSchedulerMetadata buildAnalysisSchedulerMetadata(
    std::span<const int> internalNodeIdsInOrder,
    std::span<const int> destinationIdsInOrder,
    int maxNodeId)
{
    if (maxNodeId <= 1) {
        throw std::invalid_argument("maxNodeId must be greater than 1");
    }

    if (internalNodeIdsInOrder.empty()) {
        throw std::invalid_argument("internalNodeIdsInOrder must not be empty");
    }

    AnalysisSchedulerMetadata metadata;
    metadata.internalNodeIdsInOrder.assign(internalNodeIdsInOrder.begin(), internalNodeIdsInOrder.end());
    metadata.destinationIdsInOrder.assign(destinationIdsInOrder.begin(), destinationIdsInOrder.end());
    metadata.nodeIdToInternalIndex.assign(static_cast<std::size_t>(maxNodeId), -1);
    metadata.frontierCapacity = metadata.internalNodeIdsInOrder.size();
    metadata.maxNodeId = maxNodeId;

    for (std::size_t index = 0; index < metadata.internalNodeIdsInOrder.size(); ++index) {
        const int nodeId = metadata.internalNodeIdsInOrder[index];
        if (nodeId <= 0 || nodeId >= maxNodeId) {
            throw std::invalid_argument("internal node ID out of range");
        }

        metadata.nodeIdToInternalIndex[static_cast<std::size_t>(nodeId)] = static_cast<int>(index);
    }

    for (const int destinationId : metadata.destinationIdsInOrder) {
        if (destinationId <= 0 || destinationId >= maxNodeId) {
            throw std::invalid_argument("destination node ID out of range");
        }
    }

    return metadata;
}

AnalysisRuntimeState buildAnalysisRuntimeState(const AnalysisSchedulerMetadata& metadata)
{
    AnalysisRuntimeState state;
    state.transportedValues.assign(static_cast<std::size_t>(metadata.maxNodeId), 0.0);
    state.arrivalGeneration.assign(static_cast<std::size_t>(metadata.maxNodeId), 0);
    state.frontierInternalWorkItems.assign(metadata.frontierCapacity, AnalysisWorkItem{});
    return state;
}

void beginAnalysisSample(AnalysisRuntimeState& state) noexcept
{
    ++state.currentGeneration;
    state.frontierHead = 0;
    state.frontierSize = 0;
}

void recordArrival(AnalysisRuntimeState& state, int nodeId, double sampleValue) noexcept
{
    state.transportedValues[static_cast<std::size_t>(nodeId)] = sampleValue;
    state.arrivalGeneration[static_cast<std::size_t>(nodeId)] = state.currentGeneration;
}

bool hasCurrentSampleArrival(const AnalysisRuntimeState& state, int nodeId) noexcept
{
    return state.arrivalGeneration[static_cast<std::size_t>(nodeId)] == state.currentGeneration;
}

void appendProcessedParentInternalChildren(
    AnalysisRuntimeState& state,
    int parentNodeId,
    std::optional<AnalysisWorkItem> lowChild,
    std::optional<AnalysisWorkItem> highChild) noexcept
{
    if (lowChild.has_value()) {
        if (!isExpectedChildOfParent(parentNodeId, lowChild->nodeId, true)) {
            return;
        }

        const std::size_t insertIndex = ringIndex(state, state.frontierSize);
        state.frontierInternalWorkItems[insertIndex] = *lowChild;
        ++state.frontierSize;
    }

    if (highChild.has_value()) {
        if (!isExpectedChildOfParent(parentNodeId, highChild->nodeId, false)) {
            return;
        }

        const std::size_t insertIndex = ringIndex(state, state.frontierSize);
        state.frontierInternalWorkItems[insertIndex] = *highChild;
        ++state.frontierSize;
    }
}

AnalysisWorkItem dequeueInternalArrival(AnalysisRuntimeState& state) noexcept
{
    const AnalysisWorkItem item = state.frontierInternalWorkItems[state.frontierHead];
    state.frontierHead = (state.frontierHead + 1) % state.frontierInternalWorkItems.size();
    --state.frontierSize;
    return item;
}

} // namespace dtcwpt
