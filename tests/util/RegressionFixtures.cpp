#include "RegressionFixtures.h"

#include "TestUtils.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace RegressionFixtures {

namespace {

int getLeafDepth(const std::string& destination)
{
    return static_cast<int>(destination.size());
}

bool hasMixedDepthShape(const std::vector<std::string>& destinations, int expectedMaxDepth)
{
    if (destinations.empty()) {
        return false;
    }

    int minDepth = getLeafDepth(destinations.front());
    int maxDepth = minDepth;

    for (const auto& destination : destinations) {
        const int depth = getLeafDepth(destination);
        minDepth = std::min(minDepth, depth);
        maxDepth = std::max(maxDepth, depth);
    }

    return minDepth < maxDepth && maxDepth == expectedMaxDepth;
}

std::vector<std::string> buildMixedDepthDestinations(int depth, unsigned int seed)
{
    if (depth < 2) {
        throw std::invalid_argument("Mixed-depth topology family requires depth >= 2");
    }

    const auto fullTree = TestUtils::getFullPacketDestinations(depth);
    const auto waveletTree = TestUtils::getWaveletTreeDestinations(depth);

    for (unsigned int attempt = 0; attempt < 128U; ++attempt) {
        const unsigned int candidateSeed = seed + (attempt * 7919U);
        auto candidate = TestUtils::getRandomPacketDestinations(depth, candidateSeed);

        if (! TestUtils::validateDestinations(candidate)) {
            continue;
        }

        if (candidate == fullTree || candidate == waveletTree) {
            continue;
        }

        if (! hasMixedDepthShape(candidate, depth)) {
            continue;
        }

        return candidate;
    }

    throw std::invalid_argument("Unable to build deterministic mixed-depth topology case");
}

} // namespace

std::vector<RandomTopologyCase> buildDeterministicTopologyMatrix(
    TopologyFamily family,
    std::span<const int> depths,
    std::span<const unsigned int> seeds)
{
    std::vector<RandomTopologyCase> topologyCases;
    topologyCases.reserve(depths.size() * seeds.size());

    for (const int depth : depths) {
        if (depth <= 0) {
            throw std::invalid_argument("Topology matrix depth must be positive");
        }

        for (const unsigned int seed : seeds) {
            RandomTopologyCase topologyCase;
            topologyCase.family = family;
            topologyCase.depth = depth;
            topologyCase.seed = seed;

            switch (family) {
                case TopologyFamily::dwt:
                    topologyCase.destinations = TestUtils::getWaveletTreeDestinations(depth);
                    break;
                case TopologyFamily::fullTree:
                    topologyCase.destinations = TestUtils::getFullPacketDestinations(depth);
                    break;
                case TopologyFamily::mixedDepth:
                    topologyCase.destinations = buildMixedDepthDestinations(depth, seed);
                    break;
            }

            if (! TestUtils::validateDestinations(topologyCase.destinations)) {
                throw std::invalid_argument("Deterministic topology matrix generated an invalid topology");
            }

            topologyCases.push_back(std::move(topologyCase));
        }
    }

    return topologyCases;
}

QuantizedBufferView quantizeToFloat32(
    std::span<const double> samples,
    int numChannels,
    int numSamples)
{
    if (numChannels < 0 || numSamples < 0) {
        throw std::invalid_argument("Quantized buffer dimensions must be non-negative");
    }

    const size_t expectedSize = static_cast<size_t>(numChannels) * static_cast<size_t>(numSamples);
    if (samples.size() != expectedSize) {
        throw std::invalid_argument("Quantized buffer dimensions do not match sample count");
    }

    QuantizedBufferView quantized;
    quantized.numChannels = numChannels;
    quantized.numSamples = numSamples;
    quantized.interleaved.reserve(expectedSize);

    for (const double sample : samples) {
        quantized.interleaved.push_back(static_cast<float>(sample));
    }

    return quantized;
}

bool compareQuantizedBuffers(
    const QuantizedBufferView& expected,
    const QuantizedBufferView& actual,
    juce::String& failureMessage)
{
    failureMessage.clear();

    if (expected.numChannels != actual.numChannels) {
        failureMessage = "channel count mismatch";
        return false;
    }

    if (expected.numSamples != actual.numSamples) {
        failureMessage = "sample count mismatch";
        return false;
    }

    if (expected.interleaved.size() != actual.interleaved.size()) {
        failureMessage = "buffer size mismatch";
        return false;
    }

    for (size_t index = 0; index < expected.interleaved.size(); ++index) {
        if (expected.interleaved[index] != actual.interleaved[index]) {
            failureMessage = "sample value mismatch at interleaved index " + juce::String(static_cast<int>(index));
            return false;
        }
    }

    return true;
}

bool compareSnapshotSets(
    const SnapshotSet& expected,
    const SnapshotSet& actual,
    juce::String& failureMessage)
{
    failureMessage.clear();

    if (expected.destinationsInOrder.size() != actual.destinationsInOrder.size()) {
        failureMessage = "destination count mismatch";
        return false;
    }

    if (expected.samplesPerBandInOrder.size() != actual.samplesPerBandInOrder.size()) {
        failureMessage = "samples-per-band count mismatch";
        return false;
    }

    if (expected.bands.size() != actual.bands.size()) {
        failureMessage = "band snapshot count mismatch";
        return false;
    }

    for (size_t index = 0; index < expected.destinationsInOrder.size(); ++index) {
        if (expected.destinationsInOrder[index] != actual.destinationsInOrder[index]) {
            failureMessage = "destination order mismatch at band index " + juce::String(static_cast<int>(index));
            return false;
        }
    }

    for (size_t index = 0; index < expected.samplesPerBandInOrder.size(); ++index) {
        if (expected.samplesPerBandInOrder[index] != actual.samplesPerBandInOrder[index]) {
            failureMessage = "sample count mismatch at band index " + juce::String(static_cast<int>(index));
            return false;
        }
    }

    for (size_t index = 0; index < expected.bands.size(); ++index) {
        const auto& expectedBand = expected.bands[index];
        const auto& actualBand = actual.bands[index];
        const juce::String prefix = "band " + juce::String(static_cast<int>(index)) + " (" + expectedBand.destination + ")";

        if (expectedBand.destination != actualBand.destination) {
            failureMessage = prefix + ": destination mismatch";
            return false;
        }

        if (expectedBand.numSamples != actualBand.numSamples) {
            failureMessage = prefix + ": sample count mismatch";
            return false;
        }

        if (expectedBand.numChannels != actualBand.numChannels) {
            failureMessage = prefix + ": channel count mismatch";
            return false;
        }

        juce::String nestedFailure;
        if (! compareQuantizedBuffers(expectedBand.real, actualBand.real, nestedFailure)) {
            failureMessage = prefix + ": real buffer " + nestedFailure;
            return false;
        }

        if (! compareQuantizedBuffers(expectedBand.imag, actualBand.imag, nestedFailure)) {
            failureMessage = prefix + ": imag buffer " + nestedFailure;
            return false;
        }
    }

    return true;
}

bool compareSegmentationInvariantResults(
    const SnapshotSet& referenceSnapshots,
    const SnapshotSet& alternateSnapshots,
    const QuantizedBufferView& referenceOutput,
    const QuantizedBufferView& alternateOutput,
    juce::String& failureMessage)
{
    failureMessage.clear();

    juce::String snapshotFailure;
    if (! compareSnapshotSets(referenceSnapshots, alternateSnapshots, snapshotFailure)) {
        failureMessage = "snapshot structure mismatch: " + snapshotFailure;
        return false;
    }

    juce::String outputFailure;
    if (! compareQuantizedBuffers(referenceOutput, alternateOutput, outputFailure)) {
        failureMessage = "output mismatch: " + outputFailure;
        return false;
    }

    return true;
}

ReconstructionCheck evaluateReconstruction(
    std::span<const double> input,
    std::span<const double> output,
    int latencySamples)
{
    if (latencySamples < 0) {
        throw std::invalid_argument("Latency must be non-negative");
    }

    const size_t latency = static_cast<size_t>(latencySamples);
    if (output.size() < input.size() + latency) {
        return {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), false};
    }

    double mse = 0.0;
    double signalPower = 0.0;
    double maxAbsError = 0.0;

    for (size_t index = 0; index < input.size(); ++index) {
        const double error = input[index] - output[latency + index];
        const double absError = std::abs(error);
        mse += error * error;
        signalPower += input[index] * input[index];
        maxAbsError = std::max(maxAbsError, absError);
    }

    if (! input.empty()) {
        const double sampleCount = static_cast<double>(input.size());
        mse /= sampleCount;
        signalPower /= sampleCount;
    }

    const double normalizedMse = signalPower > 0.0 ? (mse / signalPower) : mse;
    const double mseDb = normalizedMse > 0.0
        ? 10.0 * std::log10(normalizedMse)
        : -std::numeric_limits<double>::infinity();

    constexpr double kThresholdToleranceDb = 1.0e-12;
    return {mseDb, maxAbsError, mseDb <= (kReconstructionMseThresholdDb + kThresholdToleranceDb)};
}

} // namespace RegressionFixtures
