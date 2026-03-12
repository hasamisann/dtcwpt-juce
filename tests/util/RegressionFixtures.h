#pragma once

#include <juce_core/juce_core.h>

#include <span>
#include <string>
#include <vector>

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

} // namespace RegressionFixtures
