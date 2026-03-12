#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <string>
#include <vector>

namespace topology
{
inline constexpr const char* kTopologyPropertyKey = "topology";
inline constexpr const char* kTopologySeparator = ",";

enum class PersistedTopologyState
{
    missing,
    valid,
    invalid
};

struct PersistedTopologyResolution
{
    std::vector<std::string> destinations;
    PersistedTopologyState persistedState = PersistedTopologyState::missing;
};

inline juce::String serializeTopologyDestinations (const std::vector<std::string>& destinations)
{
    juce::StringArray destArray;
    destArray.ensureStorageAllocated (static_cast<int> (destinations.size()));

    for (const auto& destination : destinations)
        destArray.add (juce::String (destination));

    return destArray.joinIntoString (kTopologySeparator);
}

inline std::vector<std::string> parseTopologyDestinations (const juce::String& serialized)
{
    std::vector<std::string> parsed;

    if (serialized.isEmpty())
        return parsed;

    const juce::StringArray tokens = juce::StringArray::fromTokens (serialized, kTopologySeparator, "");
    parsed.reserve (static_cast<std::size_t> (tokens.size()));

    for (const auto& token : tokens)
        parsed.push_back (token.toStdString());

    return parsed;
}

inline PersistedTopologyResolution resolvePersistedTopology (const juce::ValueTree& state,
                                                             const std::vector<std::string>& fallback)
{
    const auto fallbackResult = [&fallback] (PersistedTopologyState persistedState)
    {
        return PersistedTopologyResolution { fallback, persistedState };
    };

    if (! state.hasProperty (kTopologyPropertyKey))
        return fallbackResult (PersistedTopologyState::missing);

    const juce::var property = state.getProperty (kTopologyPropertyKey);
    if (!property.isString())
        return fallbackResult (PersistedTopologyState::invalid);

    const auto parsed = parseTopologyDestinations (property.toString());
    if (parsed.empty())
        return fallbackResult (PersistedTopologyState::invalid);

    return PersistedTopologyResolution { parsed, PersistedTopologyState::valid };
}
} // namespace topology
