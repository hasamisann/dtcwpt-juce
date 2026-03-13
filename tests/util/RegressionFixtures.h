#pragma once

#include <dtcwpt/dtcwpt_analysis_scheduler.h>

#include <juce_core/juce_core.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dtcwpt {
struct TopologyConfig;
}

namespace RegressionFixtures {

inline constexpr double kReconstructionMseThresholdDb = -200.0;

enum class TopologyFamily {
    dwt,
    fullTree,
    mixedDepth,
};

struct RandomTopologyCase {
    TopologyFamily family;
    int depth = 0;
    unsigned int seed = 0;
    std::vector<std::string> destinations;
};

struct SegmentationPlan {
    std::string name;
    std::vector<int> blockSizes;
};

struct QuantizedBufferView {
    int numChannels = 0;
    int numSamples = 0;
    std::vector<float> interleaved;
};

struct BandSnapshot {
    std::string destination;
    int numSamples = 0;
    int numChannels = 0;
    QuantizedBufferView real;
    QuantizedBufferView imag;
};

struct SnapshotSet {
    std::vector<std::string> destinationsInOrder;
    std::vector<int> samplesPerBandInOrder;
    std::vector<BandSnapshot> bands;
};

struct ReconstructionCheck {
    double mseDb = 0.0;
    double maxAbsError = 0.0;
    bool passesMseThreshold = false;
};

struct AnalysisSnapshotScenarioResult {
    SnapshotSet mainSnapshots;
    SnapshotSet sidechainSnapshots;
    std::vector<double> output;
    QuantizedBufferView quantizedOutput;
    int latencySamples = 0;
    int bandProcessCalls = 0;
    bool hasSidechainSnapshots = false;
};

struct SchedulerBaselineResult {
    std::vector<dtcwpt::AnalysisTraceEvent> trace;
    std::vector<int> destinationIdsInOrder;
    std::vector<std::vector<double>> perDestinationOutputs;
};

struct SchedulerEquivalenceScenarioResult {
    std::vector<dtcwpt::AnalysisTraceEvent> productionTrace;
    std::vector<int> productionDestinationIdsInOrder;
    std::vector<std::vector<double>> productionPerDestinationOutputs;
    SchedulerBaselineResult baseline;
};

std::vector<RandomTopologyCase> buildDeterministicTopologyMatrix(
    TopologyFamily family,
    std::span<const int> depths,
    std::span<const unsigned int> seeds);

QuantizedBufferView quantizeToFloat32(
    std::span<const double> samples,
    int numChannels,
    int numSamples);

bool compareQuantizedBuffers(
    const QuantizedBufferView& expected,
    const QuantizedBufferView& actual,
    juce::String& failureMessage);

bool compareSnapshotSets(
    const SnapshotSet& expected,
    const SnapshotSet& actual,
    juce::String& failureMessage);

bool compareSegmentationInvariantResults(
    const SnapshotSet& referenceSnapshots,
    const SnapshotSet& alternateSnapshots,
    const QuantizedBufferView& referenceOutput,
    const QuantizedBufferView& alternateOutput,
    juce::String& failureMessage);

ReconstructionCheck evaluateReconstruction(
    std::span<const double> input,
    std::span<const double> output,
    int latencySamples);

AnalysisSnapshotScenarioResult runAnalysisSnapshotScenario(
    const dtcwpt::TopologyConfig& config,
    std::span<const double> input,
    std::optional<std::span<const double>> sidechain,
    const SegmentationPlan& plan);

bool compareAnalysisTraces(
    std::span<const dtcwpt::AnalysisTraceEvent> expected,
    std::span<const dtcwpt::AnalysisTraceEvent> actual,
    juce::String& failureMessage);

bool comparePerDestinationOutputs(
    std::span<const int> expectedDestinationIds,
    std::span<const std::vector<double>> expectedOutputs,
    std::span<const int> actualDestinationIds,
    std::span<const std::vector<double>> actualOutputs,
    juce::String& failureMessage);

SchedulerBaselineResult runLinearScanBaseline(
    const dtcwpt::TopologyConfig& config,
    std::span<const double> input,
    dtcwpt::AnalysisPathId path);

SchedulerEquivalenceScenarioResult runSchedulerEquivalenceScenario(
    const dtcwpt::TopologyConfig& config,
    std::span<const double> input,
    std::optional<std::span<const double>> sidechain,
    dtcwpt::AnalysisPathId path);

} // namespace RegressionFixtures
