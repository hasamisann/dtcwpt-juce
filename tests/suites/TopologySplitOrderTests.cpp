#include "../../sample_plugins/common/TopologySplitOrder.h"

#include <juce_core/juce_core.h>

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
    }
};

static TopologySplitOrderTests topologySplitOrderTests;
