#include "../util/RegressionFixtures.h"
#include "../util/TestUtils.h"

#include <dtcwpt/dtcwpt_analysis_node.h>
#include <dtcwpt/dtcwpt_filter_coeffs.h>
#include <dtcwpt/dtcwpt_processor.h>
#include <dtcwpt/dtcwpt_synthesis_node.h>

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <vector>

namespace {

constexpr int kScenarioSamples = 4096;
const RegressionFixtures::SegmentationPlan kReferencePlan{"reference", {kScenarioSamples}};

dtcwpt::TopologyConfig makeConfig(const std::vector<std::string>& destinations, int maxDepth)
{
    dtcwpt::TopologyConfig config;
    config.destinations = destinations;
    config.maxDepth = maxDepth;
    return config;
}

std::vector<double> makeSineSignal(int totalSamples)
{
    std::vector<double> signal(static_cast<size_t>(totalSamples), 0.0);
    constexpr double twoPi = 6.28318530717958647692;

    for (int index = 0; index < totalSamples; ++index) {
        signal[static_cast<size_t>(index)] = 0.45 * std::sin(twoPi * static_cast<double>(index) / 31.0);
    }

    return signal;
}

std::vector<double> makeDualToneSignal(int totalSamples)
{
    std::vector<double> signal(static_cast<size_t>(totalSamples), 0.0);
    constexpr double twoPi = 6.28318530717958647692;

    for (int index = 0; index < totalSamples; ++index) {
        const double sample = static_cast<double>(index);
        signal[static_cast<size_t>(index)] = 0.28 * std::sin(twoPi * sample / 19.0)
            + 0.17 * std::cos(twoPi * sample / 43.0);
    }

    return signal;
}

std::vector<double> makeSweepSignal(int totalSamples)
{
    std::vector<double> signal(static_cast<size_t>(totalSamples), 0.0);
    constexpr double twoPi = 6.28318530717958647692;

    for (int index = 0; index < totalSamples; ++index) {
        const double t = static_cast<double>(index) / static_cast<double>(totalSamples);
        const double phase = twoPi * (8.0 * t + 120.0 * t * t);
        signal[static_cast<size_t>(index)] = 0.33 * std::sin(phase);
    }

    return signal;
}

std::vector<double> makeImpulseSignal(int totalSamples)
{
    std::vector<double> signal(static_cast<size_t>(totalSamples), 0.0);
    signal[static_cast<size_t>(totalSamples / 3)] = 1.0;
    return signal;
}

struct SignalCase {
    juce::String name;
    std::vector<double> samples;
};

std::vector<SignalCase> buildSignalCases()
{
    return {
        {"sine", makeSineSignal(kScenarioSamples)},
        {"dual-tone", makeDualToneSignal(kScenarioSamples)},
        {"sweep", makeSweepSignal(kScenarioSamples)},
        {"impulse", makeImpulseSignal(kScenarioSamples)},
    };
}

struct TopologyCase {
    juce::String familyName;
    dtcwpt::TopologyConfig config;
};

std::vector<TopologyCase> buildTopologyCasesForDepth(int depth)
{
    using TopologyFamily = RegressionFixtures::TopologyFamily;
    const std::array<int, 1> depths{depth};
    std::vector<TopologyCase> topologyCases;

    const auto dwtCases = RegressionFixtures::buildDeterministicTopologyMatrix(TopologyFamily::dwt, depths, std::array<unsigned int, 1>{101U});
    topologyCases.push_back({"dwt", makeConfig(dwtCases.front().destinations, depth)});

    const auto fullTreeCases = RegressionFixtures::buildDeterministicTopologyMatrix(TopologyFamily::fullTree, depths, std::array<unsigned int, 1>{211U});
    topologyCases.push_back({"full-tree", makeConfig(fullTreeCases.front().destinations, depth)});

    if (depth >= 2) {
        const unsigned int mixedSeed = static_cast<unsigned int>(307 + (depth * 17));
        const auto mixedCases = RegressionFixtures::buildDeterministicTopologyMatrix(TopologyFamily::mixedDepth, depths, std::array<unsigned int, 1>{mixedSeed});
        topologyCases.push_back({"mixed-depth", makeConfig(mixedCases.front().destinations, depth)});
    }

    return topologyCases;
}

} // namespace

class AnalysisSynthesisTests : public juce::UnitTest {
public:
    AnalysisSynthesisTests()
        : juce::UnitTest("AnalysisSynthesis")
    {
    }

    void runTest() override
    {
        beginTest("Analysis downsample");
        testAnalysisDownsample();

        beginTest("Synthesis upsample");
        testSynthesisUpsample();

        beginTest("Perfect reconstruction matrix uses the locked c07 oracle");
        testPerfectReconstructionMatrixUsesTheLockedC07Oracle();

        beginTest("Production mixed-topology mainReal trace matches the independent baseline");
        testProductionMixedTopologyMainRealTraceMatchesIndependentBaseline();
    }

private:
    class LocalTraceSink final : public dtcwpt::AnalysisTraceSink {
    public:
        void record(const dtcwpt::AnalysisTraceEvent& event) noexcept override
        {
            events.push_back(event);
        }

        std::vector<dtcwpt::AnalysisTraceEvent> events;
    };

    static std::vector<dtcwpt::AnalysisTraceEvent> filterTraceByPath(
        std::span<const dtcwpt::AnalysisTraceEvent> trace,
        dtcwpt::AnalysisPathId path)
    {
        std::vector<dtcwpt::AnalysisTraceEvent> filtered;
        for (const auto& event : trace) {
            if (event.path == path) {
                filtered.push_back(event);
            }
        }
        return filtered;
    }

    void testAnalysisDownsample()
    {
        using namespace dtcwpt;
        using namespace dtcwpt::filters;

        AnalysisNode node(CDF_RE, true);
        constexpr size_t inputSize = 256;
        constexpr size_t expectedOutputSize = inputSize / 2;

        std::vector<double> workBuffer(inputSize * 2, 0.0);
        std::vector<char> activeFlags(inputSize * 2, 0);
        size_t outputCount = 0;

        for (size_t index = 0; index < inputSize; ++index) {
            const double input = std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * static_cast<double>(index) / 44100.0);
            if (node.updateBuffer(input, workBuffer, activeFlags, static_cast<int>(index), static_cast<int>(index + inputSize))) {
                ++outputCount;
            }
        }

        expectEquals(outputCount, expectedOutputSize,
                     "Analysis should produce one low/high output pair for every two inputs");
    }

    void testSynthesisUpsample()
    {
        using namespace dtcwpt;
        using namespace dtcwpt::filters;

        SynthesisNode node(CDF_RE, true);
        constexpr size_t inputSize = 128;
        constexpr size_t expectedOutputSize = inputSize * 2;

        std::vector<double> lowBuffer(inputSize, 0.1);
        std::vector<double> highBuffer(inputSize, 0.0);
        std::vector<double> outputBuffer(expectedOutputSize * 2, 0.0);
        node.synthesizeBlock(lowBuffer, highBuffer, outputBuffer, 0, inputSize);

        size_t nonZeroCount = 0;
        for (size_t index = 0; index < expectedOutputSize; ++index) {
            if (std::abs(outputBuffer[index]) > 1.0e-10) {
                ++nonZeroCount;
            }
        }

        expect(nonZeroCount > 0, "Synthesis should write reconstructed samples into the output buffer");
    }

    void testPerfectReconstructionMatrixUsesTheLockedC07Oracle()
    {
        const auto signals = buildSignalCases();

        for (int depth = 1; depth <= 12; ++depth) {
            const auto topologyCases = buildTopologyCasesForDepth(depth);

            for (const auto& topologyCase : topologyCases) {
                for (const auto& signalCase : signals) {
                    const auto scenario = RegressionFixtures::runAnalysisSnapshotScenario(topologyCase.config,
                                                                                          signalCase.samples,
                                                                                          std::nullopt,
                                                                                          kReferencePlan);
                    const auto reconstruction = RegressionFixtures::evaluateReconstruction(signalCase.samples,
                                                                                           scenario.output,
                                                                                           scenario.latencySamples);

                    const juce::String label = "depth " + juce::String(depth)
                        + " " + topologyCase.familyName
                        + " " + signalCase.name;

                    expect(scenario.bandProcessCalls > 0,
                           label + " should exercise at least one analysis snapshot arrival");
                    expect(reconstruction.passesMseThreshold,
                           label + " should reconstruct at <= -200 dB MSE, got "
                               + juce::String(reconstruction.mseDb, 6)
                               + " dB with max abs error "
                               + juce::String(reconstruction.maxAbsError, 12));
                }
            }
        }
    }

    void testProductionMixedTopologyMainRealTraceMatchesIndependentBaseline()
    {
#if DTCWPT_ENABLE_TEST_SEAMS
        const auto config = makeConfig({"L", "HL", "HH"}, 2);
        const std::array<double, 4> input{0.25, 0.0, 0.0, 0.0};

        LocalTraceSink sink;
        dtcwpt::DTCWPTProcessor processor;
        processor.setAnalysisTraceSinkForTesting(&sink);
        processor.prepareToPlay(TestUtils::TestConfig::SAMPLE_RATE, 4, config, 1);

        juce::AudioBuffer<double> buffer(1, 4);
        auto* writePtr = buffer.getWritePointer(0);
        for (int index = 0; index < 4; ++index) {
            writePtr[index] = input[static_cast<std::size_t>(index)];
        }
        processor.processBlock(buffer);

        const auto productionMainReal = filterTraceByPath(sink.events, dtcwpt::AnalysisPathId::mainReal);
        const auto baseline = RegressionFixtures::runLinearScanBaseline(
            config,
            input,
            dtcwpt::AnalysisPathId::mainReal);

        juce::String failureMessage;
        expect(RegressionFixtures::compareAnalysisTraces(productionMainReal, baseline.trace, failureMessage),
               "Production mainReal trace should match the independent baseline for the mixed topology: " + failureMessage);
        expect(baseline.destinationIdsInOrder == std::vector<int>({2, 6, 7}),
               "Baseline should preserve destination order for the mixed topology");
#else
        expect(false, "DTCWPT_ENABLE_TEST_SEAMS must be enabled for mixed-topology trace comparison");
#endif
    }
};

static AnalysisSynthesisTests analysisSynthesisTests;
