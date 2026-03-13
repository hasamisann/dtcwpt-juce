/**
 * @file AnalysisNodeTests.cpp
 * @brief JUCE UnitTest suite for AnalysisNode arrival processing APIs.
 */

#include "../../dtcwpt/dtcwpt_analysis_node.h"
#include "../../dtcwpt/dtcwpt_analysis_node_factory.h"
#include "../../dtcwpt/dtcwpt_filter_coeffs.h"
#include "../util/AnalysisArrivalOracle.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

class AnalysisNodeTests : public juce::UnitTest
{
public:
    AnalysisNodeTests() : UnitTest("AnalysisNode") {}

    void runTest() override
    {
        beginTest("processArrival emits on first arrival, suppresses on second, emits on third");
        testProcessArrivalParityProgression();

        beginTest("processArrival matches legacy adapter for emit/suppress and low/high values");
        testProcessArrivalMatchesLegacyAdapter();

        beginTest("Zero-valued arrivals advance parity and produce outputs matching legacy adapter");
        testZeroValuedArrivalsMatchLegacyAdapter();

        beginTest("Shared analysis-node factory builds nodes matching current filter-family rules");
        testSharedFactoryMatchesFilterFamilyRules();
    }

private:
    static constexpr double kTolerance = 1.0e-15;

    static void expectOutputsClose(juce::UnitTest& test,
                                   const dtcwpt::AnalysisChildOutputs& actual,
                                   const dtcwpt::AnalysisChildOutputs& expected,
                                   const juce::String& message)
    {
        test.expect(std::abs(actual.low - expected.low) <= kTolerance,
                    message + " (low mismatch)");
        test.expect(std::abs(actual.high - expected.high) <= kTolerance,
                    message + " (high mismatch)");
    }

    static void expectNodeMatchesDirectConstruction(juce::UnitTest& test,
                                                    int nodeId,
                                                    dtcwpt::AnalysisTreeKind treeKind,
                                                    const dtcwpt::filters::FilterCoefficients& coefficients,
                                                    const std::vector<double>& sequence,
                                                    const juce::String& label)
    {
        dtcwpt::AnalysisNode factoryNode = dtcwpt::createAnalysisNodeForId(nodeId, treeKind);
        dtcwpt::AnalysisNode directNode(coefficients, nodeId == 1);

        for (double sample : sequence) {
            dtcwpt::AnalysisChildOutputs factoryOutputs{
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
            dtcwpt::AnalysisChildOutputs directOutputs{
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};

            const bool factoryEmitted = factoryNode.processArrival(sample, factoryOutputs);
            const bool directEmitted = directNode.processArrival(sample, directOutputs);

            test.expect(factoryEmitted == directEmitted, label + " emitted mismatch");
            if (factoryEmitted && directEmitted) {
                expectOutputsClose(test, factoryOutputs, directOutputs, label);
            }
        }
    }

    void testProcessArrivalParityProgression()
    {
        dtcwpt::AnalysisNode rootNode = dtcwpt::createAnalysisNodeForId(1, dtcwpt::AnalysisTreeKind::real);
        dtcwpt::AnalysisNode nonRootNode = dtcwpt::createAnalysisNodeForId(2, dtcwpt::AnalysisTreeKind::imag);

        dtcwpt::AnalysisChildOutputs outputs{};

        expect(rootNode.processArrival(0.25, outputs), "Root node should emit on first arrival");
        expect(!rootNode.processArrival(-0.5, outputs), "Root node should suppress on second arrival");
        expect(rootNode.processArrival(0.75, outputs), "Root node should emit on third arrival");

        expect(nonRootNode.processArrival(0.25, outputs), "Non-root node should emit on first arrival");
        expect(!nonRootNode.processArrival(-0.5, outputs), "Non-root node should suppress on second arrival");
        expect(nonRootNode.processArrival(0.75, outputs), "Non-root node should emit on third arrival");
    }

    void testProcessArrivalMatchesLegacyAdapter()
    {
        dtcwpt::AnalysisNode productionNode = dtcwpt::createAnalysisNodeForId(3, dtcwpt::AnalysisTreeKind::real);
        dtcwpt::AnalysisNode legacyNode = dtcwpt::createAnalysisNodeForId(3, dtcwpt::AnalysisTreeKind::real);
        const std::vector<double> samples{0.25, -0.5, 0.0, 1.25, -0.75, 0.5};

        for (double sample : samples) {
            dtcwpt::AnalysisChildOutputs productionOutputs{
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
            dtcwpt::AnalysisChildOutputs legacyOutputs{
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};

            const bool productionEmitted = productionNode.processArrival(sample, productionOutputs);
            const bool legacyEmitted = dtcwpt::test::runLegacyAnalysisArrival(legacyNode, sample, legacyOutputs);

            expect(productionEmitted == legacyEmitted, "Emit/suppress decision must match legacy adapter");
            if (productionEmitted && legacyEmitted) {
                expectOutputsClose(*this, productionOutputs, legacyOutputs,
                                   "Direct-return API must match legacy adapter outputs");
            }
        }
    }

    void testZeroValuedArrivalsMatchLegacyAdapter()
    {
        dtcwpt::AnalysisNode productionNode = dtcwpt::createAnalysisNodeForId(1, dtcwpt::AnalysisTreeKind::imag);
        dtcwpt::AnalysisNode legacyNode = dtcwpt::createAnalysisNodeForId(1, dtcwpt::AnalysisTreeKind::imag);

        dtcwpt::AnalysisChildOutputs firstProductionOutputs{
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN()};
        dtcwpt::AnalysisChildOutputs firstLegacyOutputs{
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN()};

        const bool firstProduction = productionNode.processArrival(0.0, firstProductionOutputs);
        const bool firstLegacy = dtcwpt::test::runLegacyAnalysisArrival(legacyNode, 0.0, firstLegacyOutputs);
        expect(firstProduction == firstLegacy, "First zero arrival emit state must match legacy adapter");
        expect(firstProduction, "First zero arrival should still emit");
        expectOutputsClose(*this, firstProductionOutputs, firstLegacyOutputs,
                           "First zero arrival outputs must match legacy adapter");

        dtcwpt::AnalysisChildOutputs secondProductionOutputs{
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN()};
        dtcwpt::AnalysisChildOutputs secondLegacyOutputs{
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN()};

        const bool secondProduction = productionNode.processArrival(0.0, secondProductionOutputs);
        const bool secondLegacy = dtcwpt::test::runLegacyAnalysisArrival(legacyNode, 0.0, secondLegacyOutputs);
        expect(secondProduction == secondLegacy, "Second zero arrival emit state must match legacy adapter");
        expect(!secondProduction, "Second zero arrival should still suppress");

        dtcwpt::AnalysisChildOutputs thirdProductionOutputs{
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN()};
        dtcwpt::AnalysisChildOutputs thirdLegacyOutputs{
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN()};

        const bool thirdProduction = productionNode.processArrival(0.0, thirdProductionOutputs);
        const bool thirdLegacy = dtcwpt::test::runLegacyAnalysisArrival(legacyNode, 0.0, thirdLegacyOutputs);
        expect(thirdProduction == thirdLegacy, "Third zero arrival emit state must match legacy adapter");
        expect(thirdProduction, "Third zero arrival should emit again after parity advances");
        expectOutputsClose(*this, thirdProductionOutputs, thirdLegacyOutputs,
                           "Third zero arrival outputs must match legacy adapter");
    }

    void testSharedFactoryMatchesFilterFamilyRules()
    {
        const std::vector<double> sequence{0.125, -0.25, 0.5, -0.75, 1.0, -0.125};

        expectNodeMatchesDirectConstruction(*this, 1, dtcwpt::AnalysisTreeKind::real,
                                            dtcwpt::filters::CDF_RE, sequence,
                                            "node 1 real should use CDF_RE");
        expectNodeMatchesDirectConstruction(*this, 1, dtcwpt::AnalysisTreeKind::imag,
                                            dtcwpt::filters::CDF_IM, sequence,
                                            "node 1 imag should use CDF_IM");
        expectNodeMatchesDirectConstruction(*this, 2, dtcwpt::AnalysisTreeKind::real,
                                            dtcwpt::filters::QSHIFT14_RE, sequence,
                                            "node 2 real should use QSHIFT14_RE");
        expectNodeMatchesDirectConstruction(*this, 2, dtcwpt::AnalysisTreeKind::imag,
                                            dtcwpt::filters::QSHIFT14_IM, sequence,
                                            "node 2 imag should use QSHIFT14_IM");
        expectNodeMatchesDirectConstruction(*this, 3, dtcwpt::AnalysisTreeKind::real,
                                            dtcwpt::filters::PACKET, sequence,
                                            "node 3 real should use PACKET");
        expectNodeMatchesDirectConstruction(*this, 3, dtcwpt::AnalysisTreeKind::imag,
                                            dtcwpt::filters::PACKET, sequence,
                                            "node 3 imag should use PACKET");
    }
};

static AnalysisNodeTests analysisNodeTestsInstance;
