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

            const auto resolved = topology::resolvePersistedTopology (state, fallback);

            expect (resolved.persistedState == topology::PersistedTopologyState::valid);
            expectEquals (static_cast<int> (resolved.destinations.size()), 4);
            expect (resolved.destinations[0] == std::string { "H" });
            expect (resolved.destinations[1] == std::string { "LH" });
            expect (resolved.destinations[2] == std::string { "LLH" });
            expect (resolved.destinations[3] == std::string { "LLLL" });
        }

        beginTest ("Reports missing persisted topology when property is absent");
        {
            juce::ValueTree state { "State" };
            const std::vector<std::string> fallback { "HH", "HL", "L" };

            const auto resolved = topology::resolvePersistedTopology (state, fallback);
            expect (resolved.persistedState == topology::PersistedTopologyState::missing);
            expect (resolved.destinations == fallback);
        }

        beginTest ("Reports invalid persisted topology when string is empty");
        {
            const std::vector<std::string> fallback { "HH", "HLL", "L" };
            juce::ValueTree state { "State" };
            state.setProperty (topology::kTopologyPropertyKey, "", nullptr);

            const auto resolved = topology::resolvePersistedTopology (state, fallback);
            expect (resolved.persistedState == topology::PersistedTopologyState::invalid);
            expect (resolved.destinations == fallback);
        }

        beginTest ("Reports invalid persisted topology when property is non-string");
        {
            const std::vector<std::string> fallback { "HH", "HLL", "L" };

            juce::ValueTree state { "State" };
            state.setProperty (topology::kTopologyPropertyKey, 123, nullptr);

            const auto resolved = topology::resolvePersistedTopology (state, fallback);
            expect (resolved.persistedState == topology::PersistedTopologyState::invalid);
            expect (resolved.destinations == fallback);
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
