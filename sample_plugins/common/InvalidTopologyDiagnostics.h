#pragma once

#include <juce_core/juce_core.h>

#include <atomic>

namespace topology
{
inline constexpr const char* kInvalidTopologyDiagnosticCategory = "invalid-topology";

inline std::atomic<int>& invalidTopologyDiagnosticEmissionCount() noexcept
{
    static std::atomic<int> emissionCount { 0 };
    return emissionCount;
}

inline std::atomic<const char*>& lastInvalidTopologyDiagnosticCategory() noexcept
{
    static std::atomic<const char*> lastCategory { nullptr };
    return lastCategory;
}

inline const char* invalidTopologyDiagnosticCategory() noexcept
{
    return kInvalidTopologyDiagnosticCategory;
}

inline const char* invalidTopologyDiagnosticCategoryForCaller (const char* callerLabel) noexcept
{
    juce::ignoreUnused (callerLabel);
    return invalidTopologyDiagnosticCategory();
}

inline void emitInvalidTopologyDiagnostic (const char* callerLabel) noexcept
{
    const char* const category = invalidTopologyDiagnosticCategoryForCaller (callerLabel);
    lastInvalidTopologyDiagnosticCategory().store (category, std::memory_order_relaxed);
    invalidTopologyDiagnosticEmissionCount().fetch_add (1, std::memory_order_relaxed);

   #if JUCE_DEBUG
    juce::ignoreUnused (callerLabel);
    jassertfalse;
   #endif
}

inline void resetInvalidTopologyDiagnosticsForTest() noexcept
{
    lastInvalidTopologyDiagnosticCategory().store (nullptr, std::memory_order_relaxed);
    invalidTopologyDiagnosticEmissionCount().store (0, std::memory_order_relaxed);
}

inline int getInvalidTopologyDiagnosticEmissionCountForTest() noexcept
{
    return invalidTopologyDiagnosticEmissionCount().load (std::memory_order_relaxed);
}

inline const char* getLastInvalidTopologyDiagnosticCategoryForTest() noexcept
{
    const char* const category = lastInvalidTopologyDiagnosticCategory().load (std::memory_order_relaxed);
    return category != nullptr ? category : "";
}
} // namespace topology
