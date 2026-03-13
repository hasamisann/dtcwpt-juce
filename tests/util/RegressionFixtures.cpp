#include "RegressionFixtures.h"

#include "AnalysisArrivalOracle.h"

#include "TestUtils.h"

#include <dtcwpt/dtcwpt_analysis_node_factory.h>
#include <dtcwpt/dtcwpt_band_processor.h>
#include <dtcwpt/dtcwpt_processor.h>
#include <dtcwpt/dtcwpt_topology_planner.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace RegressionFixtures {

namespace {

dtcwpt::AnalysisTreeKind treeKindFromPath(dtcwpt::AnalysisPathId path) noexcept
{
    switch (path) {
        case dtcwpt::AnalysisPathId::mainReal:
        case dtcwpt::AnalysisPathId::sidechainReal:
            return dtcwpt::AnalysisTreeKind::real;
        case dtcwpt::AnalysisPathId::mainImag:
        case dtcwpt::AnalysisPathId::sidechainImag:
            return dtcwpt::AnalysisTreeKind::imag;
    }

    return dtcwpt::AnalysisTreeKind::real;
}

int getLeafDepth(const std::string& destination)
{
    return static_cast<int>(destination.size());
}

std::string pathFromNodeId(int nodeId)
{
    if (nodeId <= 0) {
        throw std::invalid_argument("Destination node ID must be positive");
    }

    std::string path;
    while (nodeId > 1) {
        path.push_back((nodeId & 1) != 0 ? 'H' : 'L');
        nodeId >>= 1;
    }

    std::reverse(path.begin(), path.end());
    return path;
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

class SnapshotCaptureProcessor final : public dtcwpt::BandProcessor {
public:
    void setCaptureEnabled(bool shouldCapture) noexcept
    {
        captureEnabled = shouldCapture;
    }

    void prepare(double, int, int, int) override
    {
        reset();
    }

    void processAllBands(dtcwpt::BandData& data) override
    {
        if (! captureEnabled) {
            return;
        }

        ++bandProcessCalls;
        appendBands(mainSnapshots, data.bands, data.numChannels, data.numBands, data.destinationIds);

        if (data.hasSidechain) {
            hasSidechainSnapshots = true;
            appendBands(sidechainSnapshots, data.sidechainBands, data.numChannels, data.numBands, data.destinationIds);
        }
    }

    void reset() override
    {
        mainSnapshots = {};
        sidechainSnapshots = {};
        bandProcessCalls = 0;
        hasSidechainSnapshots = false;
        captureEnabled = true;
    }

    AnalysisSnapshotScenarioResult buildResult(std::vector<double> output,
                                              const QuantizedBufferView& quantizedOutput,
                                              int latencySamples) const
    {
        AnalysisSnapshotScenarioResult result;
        result.mainSnapshots = mainSnapshots;
        result.sidechainSnapshots = sidechainSnapshots;
        result.output = std::move(output);
        result.quantizedOutput = quantizedOutput;
        result.latencySamples = latencySamples;
        result.bandProcessCalls = bandProcessCalls;
        result.hasSidechainSnapshots = hasSidechainSnapshots;
        return result;
    }

private:
    static void initializeSnapshotSet(SnapshotSet& snapshotSet,
                                      const std::vector<int>* destinationIds,
                                      int numBands,
                                      int numChannels)
    {
        if (! snapshotSet.bands.empty()) {
            return;
        }

        if (destinationIds == nullptr) {
            throw std::invalid_argument("Snapshot capture requires destination metadata");
        }

        snapshotSet.destinationsInOrder.reserve(static_cast<size_t>(numBands));
        snapshotSet.samplesPerBandInOrder.assign(static_cast<size_t>(numBands), 0);
        snapshotSet.bands.reserve(static_cast<size_t>(numBands));

        for (int bandIndex = 0; bandIndex < numBands; ++bandIndex) {
            const auto destination = pathFromNodeId(destinationIds->at(static_cast<size_t>(bandIndex)));
            snapshotSet.destinationsInOrder.push_back(destination);

            BandSnapshot bandSnapshot;
            bandSnapshot.destination = destination;
            bandSnapshot.numChannels = numChannels;
            bandSnapshot.real.numChannels = numChannels;
            bandSnapshot.imag.numChannels = numChannels;
            snapshotSet.bands.push_back(std::move(bandSnapshot));
        }
    }

    static void appendBands(SnapshotSet& snapshotSet,
                            const std::vector<std::vector<dtcwpt::ChannelBandView>>& channelBands,
                            int numChannels,
                            int numBands,
                            const std::vector<int>* destinationIds)
    {
        if (numChannels <= 0 || numBands <= 0) {
            return;
        }

        initializeSnapshotSet(snapshotSet, destinationIds, numBands, numChannels);

        for (int bandIndex = 0; bandIndex < numBands; ++bandIndex) {
            const size_t bandOffset = static_cast<size_t>(bandIndex);
            const size_t numSamples = channelBands.at(0).at(bandOffset).numSamples;

            std::vector<double> realInterleaved;
            std::vector<double> imagInterleaved;
            realInterleaved.reserve(static_cast<size_t>(numChannels) * numSamples);
            imagInterleaved.reserve(static_cast<size_t>(numChannels) * numSamples);

            for (size_t sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex) {
                for (int channelIndex = 0; channelIndex < numChannels; ++channelIndex) {
                    const auto& view = channelBands.at(static_cast<size_t>(channelIndex)).at(bandOffset);
                    realInterleaved.push_back(view.re[sampleIndex]);
                    imagInterleaved.push_back(view.im[sampleIndex]);
                }
            }

            auto& bandSnapshot = snapshotSet.bands[bandOffset];
            const int appendedSamples = static_cast<int>(numSamples);
            bandSnapshot.numSamples += appendedSamples;
            bandSnapshot.real.numSamples = bandSnapshot.numSamples;
            bandSnapshot.imag.numSamples = bandSnapshot.numSamples;
            snapshotSet.samplesPerBandInOrder[bandOffset] += appendedSamples;

            const auto quantizedReal = quantizeToFloat32(realInterleaved, numChannels, appendedSamples);
            const auto quantizedImag = quantizeToFloat32(imagInterleaved, numChannels, appendedSamples);
            bandSnapshot.real.interleaved.insert(bandSnapshot.real.interleaved.end(),
                                                 quantizedReal.interleaved.begin(),
                                                 quantizedReal.interleaved.end());
            bandSnapshot.imag.interleaved.insert(bandSnapshot.imag.interleaved.end(),
                                                 quantizedImag.interleaved.begin(),
                                                 quantizedImag.interleaved.end());
        }
    }

    SnapshotSet mainSnapshots;
    SnapshotSet sidechainSnapshots;
    int bandProcessCalls = 0;
    bool hasSidechainSnapshots = false;
    bool captureEnabled = true;
};

struct LinearBaselineTraceSink final : public dtcwpt::AnalysisTraceSink {
    void record(const dtcwpt::AnalysisTraceEvent& event) noexcept override
    {
        events.push_back(event);
    }

    std::vector<dtcwpt::AnalysisTraceEvent> events;
};

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

bool compareAnalysisTraces(
    std::span<const dtcwpt::AnalysisTraceEvent> expected,
    std::span<const dtcwpt::AnalysisTraceEvent> actual,
    juce::String& failureMessage)
{
    failureMessage.clear();

    if (expected.size() != actual.size()) {
        failureMessage = "trace event count mismatch";
        return false;
    }

    constexpr double kTraceTolerance = 1.0e-15;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& expectedEvent = expected[index];
        const auto& actualEvent = actual[index];

        if (expectedEvent.kind != actualEvent.kind) {
            failureMessage = "trace kind mismatch at event " + juce::String(static_cast<int>(index));
            return false;
        }
        if (expectedEvent.path != actualEvent.path) {
            failureMessage = "trace path mismatch at event " + juce::String(static_cast<int>(index));
            return false;
        }
        if (expectedEvent.channel != actualEvent.channel) {
            failureMessage = "trace channel mismatch at event " + juce::String(static_cast<int>(index));
            return false;
        }
        if (expectedEvent.sampleIndex != actualEvent.sampleIndex) {
            failureMessage = "trace sample index mismatch at event " + juce::String(static_cast<int>(index));
            return false;
        }
        if (expectedEvent.nodeId != actualEvent.nodeId) {
            failureMessage = "trace node ID mismatch at event " + juce::String(static_cast<int>(index));
            return false;
        }
        if (expectedEvent.relatedNodeId != actualEvent.relatedNodeId) {
            failureMessage = "trace related node ID mismatch at event " + juce::String(static_cast<int>(index));
            return false;
        }
        if (expectedEvent.cursorBeforeWrite != actualEvent.cursorBeforeWrite) {
            failureMessage = "trace cursor mismatch at event " + juce::String(static_cast<int>(index));
            return false;
        }
        if (expectedEvent.enteredFrontier != actualEvent.enteredFrontier) {
            failureMessage = "trace enteredFrontier mismatch at event " + juce::String(static_cast<int>(index));
            return false;
        }
        if (std::abs(expectedEvent.sampleValue - actualEvent.sampleValue) > kTraceTolerance) {
            failureMessage = "trace sample value mismatch at event " + juce::String(static_cast<int>(index));
            return false;
        }
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

AnalysisSnapshotScenarioResult runAnalysisSnapshotScenario(
    const dtcwpt::TopologyConfig& config,
    std::span<const double> input,
    std::optional<std::span<const double>> sidechain,
    const SegmentationPlan& plan)
{
    if (plan.blockSizes.empty()) {
        throw std::invalid_argument("Segmentation plan must contain at least one block");
    }

    const int totalSamples = std::accumulate(plan.blockSizes.begin(), plan.blockSizes.end(), 0);
    if (totalSamples <= 0) {
        throw std::invalid_argument("Segmentation plan must sum to a positive sample count");
    }

    if (static_cast<size_t>(totalSamples) != input.size()) {
        throw std::invalid_argument("Segmentation plan length must match the input sample count");
    }

    if (sidechain.has_value() && sidechain->size() != input.size()) {
        throw std::invalid_argument("Sidechain sample count must match the main input");
    }

    for (const int blockSize : plan.blockSizes) {
        if (blockSize <= 0) {
            throw std::invalid_argument("Segmentation plan blocks must be positive");
        }
    }

    const int maxBlockSize = totalSamples;

    dtcwpt::DTCWPTProcessor processor;
    auto captureProcessor = std::make_unique<SnapshotCaptureProcessor>();
    auto* captureProcessorPtr = captureProcessor.get();
    processor.setBandProcessor(std::move(captureProcessor));
    processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, maxBlockSize, config, 1);

    const int latencySamples = processor.getLatency();
    std::vector<double> output;
    output.reserve(input.size() + static_cast<size_t>(latencySamples + maxBlockSize));

    int cursor = 0;
    for (const int blockSize : plan.blockSizes) {
        juce::AudioBuffer<double> buffer(1, blockSize);
        auto* mainData = buffer.getWritePointer(0);

        for (int sampleIndex = 0; sampleIndex < blockSize; ++sampleIndex) {
            mainData[sampleIndex] = input[static_cast<size_t>(cursor + sampleIndex)];
        }

        if (sidechain.has_value()) {
            juce::AudioBuffer<double> sidechainBuffer(1, blockSize);
            auto* sidechainData = sidechainBuffer.getWritePointer(0);

            for (int sampleIndex = 0; sampleIndex < blockSize; ++sampleIndex) {
                sidechainData[sampleIndex] = sidechain.value()[static_cast<size_t>(cursor + sampleIndex)];
            }

            processor.processSidechain(sidechainBuffer);
        }

        processor.processBlock(buffer);
        const auto* outputData = buffer.getReadPointer(0);
        output.insert(output.end(), outputData, outputData + blockSize);
        cursor += blockSize;
    }

    captureProcessorPtr->setCaptureEnabled(false);

    for (int tailOffset = 0; tailOffset < latencySamples; tailOffset += maxBlockSize) {
        const int tailBlockSize = std::min(maxBlockSize, latencySamples - tailOffset);
        juce::AudioBuffer<double> buffer(1, tailBlockSize);
        buffer.clear();
        processor.processBlock(buffer);

        const auto* outputData = buffer.getReadPointer(0);
        output.insert(output.end(), outputData, outputData + tailBlockSize);
    }

    const auto quantizedOutput = quantizeToFloat32(output, 1, static_cast<int>(output.size()));
    return captureProcessorPtr->buildResult(std::move(output), quantizedOutput, latencySamples);
}

SchedulerBaselineResult runLinearScanBaseline(
    const dtcwpt::TopologyConfig& config,
    std::span<const double> input,
    dtcwpt::AnalysisPathId path)
{
    dtcwpt::TopologyPlanner planner(config.destinations);
    const auto treeKind = treeKindFromPath(path);

    std::vector<int> internalNodeIds;
    internalNodeIds.push_back(1);
    for (const int nodeId : planner.analysisOrder) {
        if (nodeId != 1) {
            internalNodeIds.push_back(nodeId);
        }
    }

    std::vector<dtcwpt::AnalysisNode> nodes;
    nodes.reserve(internalNodeIds.size());
    for (const int nodeId : internalNodeIds) {
        nodes.push_back(dtcwpt::createAnalysisNodeForId(nodeId, treeKind));
    }

    std::vector<double> workBuffer(dtcwpt::TopologyPlanner::MAX_SIZE, 0.0);
    std::vector<char> activeFlags(dtcwpt::TopologyPlanner::MAX_SIZE, 0);
    std::vector<std::vector<double>> perDestinationOutputs(planner.destinations.size());
    LinearBaselineTraceSink traceSink;

    for (int sampleIndex = 0; sampleIndex < static_cast<int>(input.size()); ++sampleIndex) {
        std::fill(activeFlags.begin(), activeFlags.end(), static_cast<char>(0));
        activeFlags[1] = 1;
        workBuffer[1] = input[static_cast<std::size_t>(sampleIndex)];

        for (std::size_t nodeIndex = 0; nodeIndex < internalNodeIds.size(); ++nodeIndex) {
            const int nodeId = internalNodeIds[nodeIndex];
            if (!activeFlags[static_cast<std::size_t>(nodeId)]) {
                continue;
            }

            traceSink.record(dtcwpt::AnalysisTraceEvent{
                .kind = dtcwpt::AnalysisTraceEventKind::dequeue,
                .path = path,
                .channel = 0,
                .sampleIndex = sampleIndex,
                .nodeId = nodeId,
                .relatedNodeId = 0,
                .cursorBeforeWrite = 0,
                .sampleValue = workBuffer[static_cast<std::size_t>(nodeId)],
                .enteredFrontier = true,
            });

            dtcwpt::AnalysisChildOutputs outputs{};
            const bool emitted = dtcwpt::test::runLegacyAnalysisArrival(
                nodes[nodeIndex],
                workBuffer[static_cast<std::size_t>(nodeId)],
                outputs);
            if (!emitted) {
                continue;
            }

            const int leftNodeId = nodeId << 1;
            const int rightNodeId = (nodeId << 1) | 1;
            workBuffer[static_cast<std::size_t>(leftNodeId)] = outputs.low;
            workBuffer[static_cast<std::size_t>(rightNodeId)] = outputs.high;
            activeFlags[static_cast<std::size_t>(leftNodeId)] = 1;
            activeFlags[static_cast<std::size_t>(rightNodeId)] = 1;

            const bool leftIsInternal = std::find(internalNodeIds.begin(), internalNodeIds.end(), leftNodeId)
                != internalNodeIds.end();
            const bool rightIsInternal = std::find(internalNodeIds.begin(), internalNodeIds.end(), rightNodeId)
                != internalNodeIds.end();

            traceSink.record(dtcwpt::AnalysisTraceEvent{
                .kind = dtcwpt::AnalysisTraceEventKind::childArrivalRecorded,
                .path = path,
                .channel = 0,
                .sampleIndex = sampleIndex,
                .nodeId = nodeId,
                .relatedNodeId = leftNodeId,
                .cursorBeforeWrite = 0,
                .sampleValue = outputs.low,
                .enteredFrontier = leftIsInternal,
            });
            traceSink.record(dtcwpt::AnalysisTraceEvent{
                .kind = dtcwpt::AnalysisTraceEventKind::childArrivalRecorded,
                .path = path,
                .channel = 0,
                .sampleIndex = sampleIndex,
                .nodeId = nodeId,
                .relatedNodeId = rightNodeId,
                .cursorBeforeWrite = 0,
                .sampleValue = outputs.high,
                .enteredFrontier = rightIsInternal,
            });
        }

        for (std::size_t destinationIndex = 0; destinationIndex < planner.destinations.size(); ++destinationIndex) {
            const int destinationId = planner.destinations[destinationIndex];
            if (!activeFlags[static_cast<std::size_t>(destinationId)]) {
                continue;
            }

            traceSink.record(dtcwpt::AnalysisTraceEvent{
                .kind = dtcwpt::AnalysisTraceEventKind::destinationWrite,
                .path = path,
                .channel = 0,
                .sampleIndex = sampleIndex,
                .nodeId = destinationId,
                .relatedNodeId = 0,
                .cursorBeforeWrite = perDestinationOutputs[destinationIndex].size(),
                .sampleValue = workBuffer[static_cast<std::size_t>(destinationId)],
                .enteredFrontier = false,
            });
            perDestinationOutputs[destinationIndex].push_back(workBuffer[static_cast<std::size_t>(destinationId)]);
        }
    }

    SchedulerBaselineResult result;
    result.trace = std::move(traceSink.events);
    result.destinationIdsInOrder = planner.destinations;
    result.perDestinationOutputs = std::move(perDestinationOutputs);
    return result;
}

} // namespace RegressionFixtures
