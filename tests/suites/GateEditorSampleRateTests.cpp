#include <juce_core/juce_core.h>

#include "../../sample_plugins/dtcwpt-gate/gui/GateEditorSampleRate.h"

class GateEditorSampleRateTests : public juce::UnitTest
{
public:
    GateEditorSampleRateTests() : juce::UnitTest ("GateEditorSampleRateTests") {}

    void runTest() override
    {
        beginTest ("Falls back to 44100 Hz when host sample rate is zero");
        {
            expectEquals (resolveGateEditorConstructionSampleRate (0.0), 44100.0);
        }

        beginTest ("Falls back to 44100 Hz when host sample rate is negative");
        {
            expectEquals (resolveGateEditorConstructionSampleRate (-48000.0), 44100.0);
        }

        beginTest ("Keeps valid positive host sample rate");
        {
            expectEquals (resolveGateEditorConstructionSampleRate (48000.0), 48000.0);
        }
    }
};

static GateEditorSampleRateTests gateEditorSampleRateTests;
