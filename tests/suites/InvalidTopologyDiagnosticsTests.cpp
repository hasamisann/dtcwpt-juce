#include "../../sample_plugins/common/InvalidTopologyDiagnostics.h"

#include <juce_core/juce_core.h>

#include <string_view>
#include <type_traits>

class InvalidTopologyDiagnosticsTests : public juce::UnitTest
{
public:
    InvalidTopologyDiagnosticsTests() : juce::UnitTest ("InvalidTopologyDiagnosticsTests") {}

    void runTest() override
    {
        beginTest ("Shared invalid-topology category token is stable and non-empty");
        {
            const auto* const token = topology::invalidTopologyDiagnosticCategory();
            expect (token != nullptr);
            expect (std::string_view { token }.size() > 0U);
            expectEquals (juce::String (token), juce::String (topology::invalidTopologyDiagnosticCategory()));
        }

        beginTest ("Caller labels share the same category token");
        {
            expectEquals (juce::String (topology::invalidTopologyDiagnosticCategoryForCaller ("Morph")),
                          juce::String (topology::invalidTopologyDiagnosticCategory()));
            expectEquals (juce::String (topology::invalidTopologyDiagnosticCategoryForCaller ("Gate")),
                          juce::String (topology::invalidTopologyDiagnosticCategory()));
        }

        beginTest ("Emission helper is noexcept and observable in tests");
        {
            static_assert (noexcept (topology::emitInvalidTopologyDiagnostic ("Morph")));
            static_assert (std::is_same_v<decltype (topology::emitInvalidTopologyDiagnostic ("Morph")), void>);

            topology::resetInvalidTopologyDiagnosticsForTest();
            expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 0);
            expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()), juce::String());

            topology::emitInvalidTopologyDiagnostic ("Morph");
            expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 1);
            expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                          juce::String (topology::invalidTopologyDiagnosticCategory()));

            topology::emitInvalidTopologyDiagnostic ("Gate");
            expectEquals (topology::getInvalidTopologyDiagnosticEmissionCountForTest(), 2);
            expectEquals (juce::String (topology::getLastInvalidTopologyDiagnosticCategoryForTest()),
                          juce::String (topology::invalidTopologyDiagnosticCategory()));
        }
    }
};

static InvalidTopologyDiagnosticsTests invalidTopologyDiagnosticsTests;
