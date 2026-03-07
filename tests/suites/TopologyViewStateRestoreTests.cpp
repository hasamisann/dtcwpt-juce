#include "../../sample_plugins/common/TopologyPersistence.h"

#include <juce_core/juce_core.h>

class TopologyViewStateRestoreTests : public juce::UnitTest
{
public:
    TopologyViewStateRestoreTests() : juce::UnitTest ("TopologyViewStateRestoreTests") {}

    void runTest() override
    {
        beginTest ("Parses valid stored topology in order");
        {
            juce::ValueTree state { "State" };
            const std::vector<std::string> fallback { "H", "L" };
            state.setProperty (topology::kTopologyPropertyKey, "H,LH,LLH,LLLL", nullptr);

            const auto resolved = topology::resolvePersistedTopologyDestinations (state, fallback);

            expectEquals (static_cast<int> (resolved.size()), 4);
            expectEquals (resolved[0], std::string { "H" });
            expectEquals (resolved[1], std::string { "LH" });
            expectEquals (resolved[2], std::string { "LLH" });
            expectEquals (resolved[3], std::string { "LLLL" });
        }

        beginTest ("Falls back when stored topology is empty");
        {
            juce::ValueTree state { "State" };
            const std::vector<std::string> fallback { "HH", "HL", "L" };
            state.setProperty (topology::kTopologyPropertyKey, "", nullptr);

            const auto resolved = topology::resolvePersistedTopologyDestinations (state, fallback);
            expect (resolved == fallback);
        }

        beginTest ("Falls back when property is missing or non-string");
        {
            const std::vector<std::string> fallback { "HH", "HLL", "L" };

            juce::ValueTree missingState { "State" };
            const auto missingResolved = topology::resolvePersistedTopologyDestinations (missingState, fallback);
            expect (missingResolved == fallback);

            juce::ValueTree nonStringState { "State" };
            nonStringState.setProperty (topology::kTopologyPropertyKey, 123, nullptr);
            const auto nonStringResolved = topology::resolvePersistedTopologyDestinations (nonStringState, fallback);
            expect (nonStringResolved == fallback);
        }

        beginTest ("Serialize then parse preserves destination order");
        {
            const std::vector<std::string> original { "H", "LH", "LLH", "LLLH", "LLLL" };
            const juce::String serialized = topology::serializeTopologyDestinations (original);
            const auto parsed = topology::parseTopologyDestinations (serialized);

            expect (parsed == original);
        }
    }
};

static TopologyViewStateRestoreTests topologyViewStateRestoreTests;
