#include "../../sample_plugins/common/TopologySplitOrder.h"

#include <dtcwpt/dtcwpt_topology_planner.h>

#include <juce_core/juce_core.h>

#include <vector>

class TopologySplitOrderTests : public juce::UnitTest
{
public:
    TopologySplitOrderTests() : juce::UnitTest ("TopologySplitOrderTests") {}

    void runTest() override
    {
        beginTest ("Parent H uses HH then HL");
        {
            const auto children = topology::makeHighFirstSplitChildren (3, "H");

            expectEquals (children.highChild.nodeId, 7);
            expectEquals (children.highChild.path, std::string { "HH" });
            expectEquals (children.lowChild.nodeId, 6);
            expectEquals (children.lowChild.path, std::string { "HL" });
        }

        beginTest ("Parent LL uses LLH then LLL");
        {
            const auto children = topology::makeHighFirstSplitChildren (4, "LL");

            expectEquals (children.highChild.nodeId, 9);
            expectEquals (children.highChild.path, std::string { "LLH" });
            expectEquals (children.lowChild.nodeId, 8);
            expectEquals (children.lowChild.path, std::string { "LLL" });
        }

        beginTest ("Planner destination order stays observable for shuffled valid leaves");
        {
            const std::vector<std::string> ordered { "LL", "LH", "HL", "HH" };
            const std::vector<std::string> shuffled { "HL", "LL", "HH", "LH" };

            dtcwpt::TopologyPlanner orderedPlanner { ordered };
            dtcwpt::TopologyPlanner shuffledPlanner { shuffled };

            expect (orderedPlanner.analysisOrder == shuffledPlanner.analysisOrder);
            expect (orderedPlanner.synthesisOrder == shuffledPlanner.synthesisOrder);
            expect (orderedPlanner.destinations != shuffledPlanner.destinations);
            expect (shuffledPlanner.destinations == std::vector<int> ({ 6, 4, 7, 5 }));
        }
    }
};

static TopologySplitOrderTests topologySplitOrderTests;
