#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace topology
{
inline constexpr const char* kTopologyPropertyKey = "topology";
inline constexpr const char* kTopologySeparator = ",";
inline constexpr int kSupportedTopologyDepth = 12;

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

namespace detail
{
struct PathShapeState
{
    bool isLeaf = false;
    bool hasLowChild = false;
    bool hasHighChild = false;
};

inline bool isValidTopologyDestinationPath (const std::string& path)
{
    if (path.empty() || static_cast<int> (path.size()) > kSupportedTopologyDepth)
        return false;

    for (const char c : path)
    {
        if (c != 'L' && c != 'H')
            return false;
    }

    return true;
}

inline bool validatePersistedTopologyDestinations (const std::vector<std::string>& destinations)
{
    if (destinations.empty())
        return false;

    std::set<std::string> uniqueDestinations;
    std::map<std::string, PathShapeState> states;

    for (const auto& destination : destinations)
    {
        if (! isValidTopologyDestinationPath (destination))
            return false;

        if (! uniqueDestinations.insert (destination).second)
            return false;

        states[destination].isLeaf = true;

        for (std::size_t prefixLength = 1; prefixLength < destination.size(); ++prefixLength)
        {
            if (uniqueDestinations.contains (destination.substr (0, prefixLength)))
                return false;
        }

        for (std::size_t prefixLength = 0; prefixLength < destination.size(); ++prefixLength)
        {
            const std::string parent = destination.substr (0, prefixLength);
            auto& state = states[parent];
            if (destination[prefixLength] == 'L')
                state.hasLowChild = true;
            else
                state.hasHighChild = true;
        }
    }

    for (const auto& [path, state] : states)
    {
        const bool hasChildren = state.hasLowChild || state.hasHighChild;
        if (state.isLeaf && hasChildren)
            return false;

        if (! state.isLeaf && (! state.hasLowChild || ! state.hasHighChild))
            return false;

        juce::ignoreUnused (path);
    }

    return true;
}
} // namespace detail

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
    if (! detail::validatePersistedTopologyDestinations (parsed))
        return fallbackResult (PersistedTopologyState::invalid);

    return PersistedTopologyResolution { parsed, PersistedTopologyState::valid };
}
} // namespace topology
