#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace statever
{
inline constexpr const char* kStateVersionPropertyKey = "stateVersion";

// Version 0: legacy data written before versioning (no stateVersion property).
// Version 1: first versioned format (APVTS state + topology property).
inline constexpr int kCurrentStateVersion = 1;

inline int readStateVersion (const juce::ValueTree& state)
{
    return static_cast<int> (state.getProperty (kStateVersionPropertyKey, 0));
}

// Entry point for upgrading older state layouts to the current one.
// Add one `if (fromVersion < N)` step per format change. Versions newer than
// kCurrentStateVersion are left untouched: only the parts this build
// understands are read (best effort).
inline void migrateState (juce::ValueTree& state, int fromVersion)
{
    juce::ignoreUnused (state);

    // 0 -> 1: no structural change, version 0 data loads as is.
    if (fromVersion < 1)
    {
    }
}

// Serialises a snapshot of the APVTS. copyState() flushes pending parameter
// values to the tree first, so changes made just before saving are included.
inline void writeVersionedState (juce::AudioProcessorValueTreeState& apvts, juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (kStateVersionPropertyKey, kCurrentStateVersion, nullptr);

    juce::MemoryOutputStream stream (destData, true);
    state.writeToStream (stream);
}

// Parses saved data. Returns an invalid tree when the data is unreadable or
// its root type differs from expectedType; callers must then leave their
// state untouched. On success the tree has been migrated to the current layout
// and no longer carries stateVersion (it is re-added when saving).
inline juce::ValueTree readVersionedState (const void* data, int sizeInBytes, const juce::Identifier& expectedType)
{
    if (data == nullptr || sizeInBytes <= 0)
        return {};

    auto state = juce::ValueTree::readFromData (data, static_cast<std::size_t> (sizeInBytes));
    if (! state.isValid() || ! state.hasType (expectedType))
        return {};

    migrateState (state, readStateVersion (state));
    state.removeProperty (kStateVersionPropertyKey, nullptr);
    return state;
}
} // namespace statever
