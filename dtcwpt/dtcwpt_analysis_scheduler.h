#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dtcwpt {

enum class AnalysisPathId {
    mainReal,
    mainImag,
    sidechainReal,
    sidechainImag,
};

enum class AnalysisTraceEventKind {
    dequeue,
    childArrivalRecorded,
    destinationWrite,
};

struct AnalysisTraceEvent {
    AnalysisTraceEventKind kind{};
    AnalysisPathId path = AnalysisPathId::mainReal;
    int channel = 0;
    int sampleIndex = 0;
    int nodeId = 0;
    int relatedNodeId = 0;
    std::size_t cursorBeforeWrite = 0;
    double sampleValue = 0.0;
    bool enteredFrontier = false;
};

class AnalysisTraceSink {
public:
    virtual ~AnalysisTraceSink() = default;
    virtual void record(const AnalysisTraceEvent& event) noexcept = 0;
};

struct AnalysisWorkItem {
    int nodeId = 0;
    double sampleValue = 0.0;
};

struct AnalysisSchedulerMetadata {
    std::vector<int> internalNodeIdsInOrder;
    std::vector<int> destinationIdsInOrder;
    std::vector<int> nodeIdToInternalIndex;
    std::size_t frontierCapacity = 0;
    int maxNodeId = 0;
};

struct AnalysisRuntimeState {
    std::vector<double> transportedValues;
    std::vector<std::uint64_t> arrivalGeneration;
    std::vector<AnalysisWorkItem> frontierInternalWorkItems;
    std::size_t frontierHead = 0;
    std::size_t frontierSize = 0;
    std::uint64_t currentGeneration = 0;
};

AnalysisSchedulerMetadata buildAnalysisSchedulerMetadata(
    std::span<const int> internalNodeIdsInOrder,
    std::span<const int> destinationIdsInOrder,
    int maxNodeId);

AnalysisRuntimeState buildAnalysisRuntimeState(
    const AnalysisSchedulerMetadata& metadata);

void beginAnalysisSample(AnalysisRuntimeState& state) noexcept;

void recordArrival(AnalysisRuntimeState& state, int nodeId, double sampleValue) noexcept;

bool hasCurrentSampleArrival(const AnalysisRuntimeState& state, int nodeId) noexcept;

void appendProcessedParentInternalChildren(
    AnalysisRuntimeState& state,
    int parentNodeId,
    std::optional<AnalysisWorkItem> lowChild,
    std::optional<AnalysisWorkItem> highChild) noexcept;

AnalysisWorkItem dequeueInternalArrival(AnalysisRuntimeState& state) noexcept;

} // namespace dtcwpt
