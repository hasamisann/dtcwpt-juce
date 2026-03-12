/**
 * @file TopologyPlannerTests.cpp
 * @brief JUCE UnitTest suite for TopologyPlanner.
 */

#include <dtcwpt/dtcwpt_topology_planner.h>
#include "../util/RegressionFixtures.h"
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <array>
#include <vector>
#include <string>

class TopologyPlannerTests : public juce::UnitTest {
public:
    TopologyPlannerTests() : UnitTest("TopologyPlanner") {}

    void runTest() override {
        beginTest("Full depth-1 topology");
        testFullDepth1();

        beginTest("Full depth-2 topology");
        testFullDepth2();

        beginTest("Mixed depth topology");
        testMixedDepth();

        beginTest("Generator validation");
        testGeneratorValidation();

        beginTest("Depth-12 full tree accepted");
        testDepth12FullTreeAccepted();

        beginTest("Out-of-range path rejected");
        testOutOfRangePathRejected();

        beginTest("Representative valid order regression");
        testRepresentativeValidOrderRegression();

        beginTest("Representative deterministic matrix cases stay planner-valid");
        testRepresentativeDeterministicMatrixCases();

        beginTest("Shuffled valid destinations preserve destination order");
        testShuffledValidDestinationsPreserveOrder();
    }

private:
    void testFullDepth1() {
        // Destinations {"L", "H"} -> analysisOrder = [1], synthesisOrder = [1]
        std::vector<std::string> destinations = {"L", "H"};
        dtcwpt::TopologyPlanner planner(destinations);

        auto [analysisOrder, synthesisOrder] = planner.getPlan();

        // Analysis order should contain node 1 (root)
        expectEquals(analysisOrder.size(), static_cast<size_t>(1), 
                     "Analysis order should have 1 node for depth-1 full packet");
        expectEquals(analysisOrder[0], 1, "Analysis order should contain root node (1)");

        // Synthesis order should contain node 1 (root)
        expectEquals(synthesisOrder.size(), static_cast<size_t>(1),
                     "Synthesis order should have 1 node for depth-1 full packet");
        expectEquals(synthesisOrder[0], 1, "Synthesis order should contain root node (1)");

        // Destinations should be nodes 2 and 3 (L=2, H=3)
        expectEquals(planner.destinations.size(), static_cast<size_t>(2),
                     "Should have 2 destination nodes");
        expect(planner.destinations[0] == 2 || planner.destinations[0] == 3,
               "First destination should be L(2) or H(3)");
        expect(planner.destinations[1] == 2 || planner.destinations[1] == 3,
               "Second destination should be L(2) or H(3)");
    }

    void testFullDepth2() {
        // Destinations {"LL", "LH", "HL", "HH"} 
        // -> analysisOrder = [1, 2, 3], synthesisOrder = [2, 3, 1]
        std::vector<std::string> destinations = {"LL", "LH", "HL", "HH"};
        dtcwpt::TopologyPlanner planner(destinations);

        auto [analysisOrder, synthesisOrder] = planner.getPlan();

        // Analysis order: root before children
        expectEquals(analysisOrder.size(), static_cast<size_t>(3),
                     "Analysis order should have 3 nodes for depth-2 full packet");
        
        // Root (1) should be first
        expectEquals(analysisOrder[0], 1, "Root should be first in analysis order");

        // Synthesis order: children before root
        expectEquals(synthesisOrder.size(), static_cast<size_t>(3),
                     "Synthesis order should have 3 nodes for depth-2 full packet");
        
        // Root (1) should be last in synthesis
        expectEquals(synthesisOrder[2], 1, "Root should be last in synthesis order");

        // Destinations should be 4 nodes (LL=4, LH=5, HL=6, HH=7)
        expectEquals(planner.destinations.size(), static_cast<size_t>(4),
                     "Should have 4 destination nodes");
    }

    void testMixedDepth() {
        // Destinations {"LL", "LH", "H"} 
        // Verify root before node 2 in analysis, node 2 before root in synthesis
        std::vector<std::string> destinations = {"LL", "LH", "H"};
        dtcwpt::TopologyPlanner planner(destinations);

        auto [analysisOrder, synthesisOrder] = planner.getPlan();

        // Find positions of root (1) and node 2 in analysis order
        auto rootPosAnalysis = std::find(analysisOrder.begin(), analysisOrder.end(), 1);
        auto node2PosAnalysis = std::find(analysisOrder.begin(), analysisOrder.end(), 2);

        expect(rootPosAnalysis != analysisOrder.end(), "Root should be in analysis order");
        expect(node2PosAnalysis != analysisOrder.end(), "Node 2 should be in analysis order");
        expect(rootPosAnalysis < node2PosAnalysis, 
               "Root should come before node 2 in analysis order");

        // Find positions in synthesis order
        auto rootPosSynthesis = std::find(synthesisOrder.begin(), synthesisOrder.end(), 1);
        auto node2PosSynthesis = std::find(synthesisOrder.begin(), synthesisOrder.end(), 2);

        expect(rootPosSynthesis != synthesisOrder.end(), "Root should be in synthesis order");
        expect(node2PosSynthesis != synthesisOrder.end(), "Node 2 should be in synthesis order");
        expect(node2PosSynthesis < rootPosSynthesis,
               "Node 2 should come before root in synthesis order");
    }

    void testGeneratorValidation() {
        // Validate all generators produce valid leaf sets at depths 1-12
        for (int depth = 1; depth <= 12; ++depth) {
            // Full packet destinations
            auto fullPacket = TestUtils::getFullPacketDestinations(depth);
            expect(TestUtils::validateDestinations(fullPacket),
                   "Full packet destinations should be valid at depth " + juce::String(depth));

            // Wavelet tree destinations
            auto waveletTree = TestUtils::getWaveletTreeDestinations(depth);
            expect(TestUtils::validateDestinations(waveletTree),
                   "Wavelet tree destinations should be valid at depth " + juce::String(depth));

            // Random packet destinations
            auto randomPacket = TestUtils::getRandomPacketDestinations(depth, 42);
            expect(TestUtils::validateDestinations(randomPacket),
                   "Random packet destinations should be valid at depth " + juce::String(depth));
        }
    }

    void testDepth12FullTreeAccepted() {
        const auto destinations = TestUtils::getFullPacketDestinations(12);

        dtcwpt::TopologyPlanner planner(destinations);
        const auto [analysisOrder, synthesisOrder] = planner.getPlan();

        expectEquals(planner.destinations.size(), static_cast<size_t>(4096),
                     "Depth-12 full packet should have 4096 destinations");
        expect(!analysisOrder.empty(), "Depth-12 analysis order should not be empty");
        expect(!synthesisOrder.empty(), "Depth-12 synthesis order should not be empty");
    }

    void testOutOfRangePathRejected() {
        const std::vector<std::string> invalidDestinations = {"LLLLLLLLLLLLL"};
        bool threwInvalidArgument = false;

        try {
            dtcwpt::TopologyPlanner planner(invalidDestinations);
        } catch (const std::invalid_argument&) {
            threwInvalidArgument = true;
        } catch (...) {
        }

        expect(threwInvalidArgument,
               "Depth-13 path should be rejected before planner indexing");
    }

    void testRepresentativeValidOrderRegression() {
        {
            const std::vector<std::string> destinations = {"L", "HL", "HH"};
            dtcwpt::TopologyPlanner planner(destinations);
            const auto [analysisOrder, synthesisOrder] = planner.getPlan();

            expect(analysisOrder == std::vector<int>({1, 3}),
                   "Mixed-depth valid topology should preserve analysis order [1, 3]");
            expect(synthesisOrder == std::vector<int>({3, 1}),
                   "Mixed-depth valid topology should preserve synthesis order [3, 1]");
            expect(planner.destinations == std::vector<int>({2, 6, 7}),
                   "Mixed-depth valid topology should preserve destination node order");
        }

        {
            const std::vector<std::string> destinations = {"LLL", "LLH", "LH", "H"};
            dtcwpt::TopologyPlanner planner(destinations);
            const auto [analysisOrder, synthesisOrder] = planner.getPlan();

            expect(analysisOrder == std::vector<int>({1, 2, 4}),
                   "Wavelet-tree topology should preserve analysis order [1, 2, 4]");
            expect(synthesisOrder == std::vector<int>({4, 2, 1}),
                   "Wavelet-tree topology should preserve synthesis order [4, 2, 1]");
            expect(planner.destinations == std::vector<int>({8, 9, 5, 3}),
                   "Wavelet-tree topology should preserve destination node order");
        }
    }

    void testRepresentativeDeterministicMatrixCases() {
        const auto dwtCases = RegressionFixtures::buildDeterministicTopologyMatrix(
            RegressionFixtures::TopologyFamily::dwt,
            std::array<int, 1>{3},
            std::array<unsigned int, 1>{11U});
        const auto fullTreeCases = RegressionFixtures::buildDeterministicTopologyMatrix(
            RegressionFixtures::TopologyFamily::fullTree,
            std::array<int, 1>{3},
            std::array<unsigned int, 1>{13U});
        const auto mixedCases = RegressionFixtures::buildDeterministicTopologyMatrix(
            RegressionFixtures::TopologyFamily::mixedDepth,
            std::array<int, 1>{4},
            std::array<unsigned int, 1>{17U});

        const std::vector<RegressionFixtures::RandomTopologyCase> cases{
            dwtCases.front(),
            fullTreeCases.front(),
            mixedCases.front()
        };

        for (const auto& topologyCase : cases) {
            dtcwpt::TopologyPlanner planner(topologyCase.destinations);
            const auto [analysisOrder, synthesisOrder] = planner.getPlan();

            expect(!analysisOrder.empty(), "Representative deterministic case should produce analysis work");
            expect(!synthesisOrder.empty(), "Representative deterministic case should produce synthesis work");
            expectEquals(static_cast<int>(planner.destinations.size()), static_cast<int>(topologyCase.destinations.size()),
                         "Planner destination count should match the deterministic topology case");
        }
    }

    void testShuffledValidDestinationsPreserveOrder() {
        const std::vector<std::string> shuffledDestinations = {"HH", "LL", "HL", "LH"};
        dtcwpt::TopologyPlanner planner(shuffledDestinations);

        expect(planner.destinations == std::vector<int>({7, 4, 6, 5}),
               "Planner destination node order should follow the caller-provided destination order");
    }
};

// Static registration
static TopologyPlannerTests topologyPlannerTests;
