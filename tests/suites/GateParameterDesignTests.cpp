#include "GateAudioProcessor.h"

#include "../../sample_plugins/dtcwpt-gate/gui/GateGuiFrequencyMapping.h"
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

/**
 * Issue #16: the Gate parameters must not depend on the topology, must be few enough to be
 * usable from a host, and must parse typed values with or without a unit.
 */
class GateParameterDesignTests : public juce::UnitTest
{
public:
    GateParameterDesignTests() : juce::UnitTest ("GateParameterDesignTests") {}

    void runTest() override
    {
        beginTest("Highest packet band uses its physical frequency range");
        {
            const auto range = gate_gui::getBandFrequencyRange("HLL", 24000.0f);
            expectEquals(range.first, 21000.0f);
            expectEquals(range.second, 24000.0f);
        }

        beginTest ("Threshold text input parses with or without a unit");
        testTextInputParsing();

        beginTest ("Threshold text output keeps the 'X.X dB' format");
        testTextOutputFormat();

        beginTest ("Host sees a practical number of threshold parameters");
        testParameterCount();

        beginTest ("The lowest-frequency threshold parameter acts on the lowest band in every topology");
        testFirstParameterActsOnLowestBandInEveryTopology();

        beginTest ("The band containing 1 kHz is controlled by the 1 kHz point in every topology");
        testOneKilohertzPointControlsItsBandInEveryTopology();
    }

private:
    static constexpr double kSampleRate = TestUtils::TestConfig::SAMPLE_RATE;
    static constexpr int kBlockSize = 512;

    //==========================================================================
    // Helpers
    //==========================================================================

    static std::unique_ptr<GateAudioProcessor> createPreparedProcessor()
    {
        auto processor = std::make_unique<GateAudioProcessor>();
        processor->prepareToPlay (kSampleRate, kBlockSize);
        return processor;
    }

    /** Threshold parameters in host order (identified by the "threshold_" ID prefix). */
    static std::vector<juce::AudioParameterFloat*> getThresholdParameters (GateAudioProcessor& processor)
    {
        std::vector<juce::AudioParameterFloat*> result;

        for (auto* parameter : processor.getParameters())
        {
            auto* floatParameter = dynamic_cast<juce::AudioParameterFloat*> (parameter);
            if (floatParameter != nullptr && floatParameter->paramID.startsWith ("threshold_"))
                result.push_back (floatParameter);
        }

        return result;
    }

    static juce::AudioParameterFloat* findFloatParameter (GateAudioProcessor& processor, const juce::String& id)
    {
        return dynamic_cast<juce::AudioParameterFloat*> (processor.getAPVTS().getParameter (id));
    }

    static void setParameterDb (juce::AudioParameterFloat& parameter, float db)
    {
        parameter.setValueNotifyingHost (parameter.convertTo0to1 (db));
    }

    static void processOneBlock (GateAudioProcessor& processor)
    {
        juce::AudioBuffer<float> buffer (2, kBlockSize);
        buffer.clear();
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
    }

    static std::vector<double> readBandThresholdsDb (const GateAudioProcessor& processor, int numBands)
    {
        std::vector<double> thresholdsDb;
        for (int band = 0; band < numBands; ++band)
            thresholdsDb.push_back (20.0 * std::log10 (processor.getLastThresholdLinearForTest (band)));
        return thresholdsDb;
    }

    static std::pair<float, float> bandRange (const std::string& path)
    {
        return gate_gui::getBandFrequencyRange (path, static_cast<float> (kSampleRate * 0.5));
    }

    /** The topologies used to show that parameter meaning does not follow the topology. */
    static std::vector<std::pair<juce::String, std::vector<std::string>>> getTopologies()
    {
        return {
            { "6-stage DWT", { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" } },
            { "depth-4 full packet", TestUtils::getFullPacketDestinations (4) },
        };
    }

    //==========================================================================
    // Tests
    //==========================================================================

    /** Real value (dB) that the host would get for typed text. */
    static float valueFromText (juce::AudioParameterFloat& parameter, const juce::String& text)
    {
        juce::AudioProcessorParameter& hostView = parameter;
        return parameter.convertFrom0to1 (hostView.getValueForText (text));
    }

    void testTextInputParsing()
    {
        GateAudioProcessor processor;
        const auto parameters = getThresholdParameters (processor);
        expect (! parameters.empty(), "Gate should expose threshold parameters");
        if (parameters.empty())
            return;

        struct Case { const char* text; float expectedDb; };
        const Case cases[] = {
            { "-30.0 dB", -30.0f },
            { "-30",      -30.0f },
            { "-30dB",    -30.0f },
            { "-6.5",      -6.5f },
            { " -12 dB",  -12.0f },
            { "-30.0 db", -30.0f },
        };

        // Every threshold shares one text function; check the first, a middle and the last parameter.
        for (auto* parameter : { parameters.front(), parameters[parameters.size() / 2], parameters.back() })
        {
            for (const auto& testCase : cases)
            {
                expectWithinAbsoluteError (valueFromText (*parameter, testCase.text), testCase.expectedDb, 0.05f,
                                           parameter->paramID + ": typing \"" + testCase.text + "\" should give "
                                               + juce::String (testCase.expectedDb, 1) + " dB");
            }
        }
    }

    void testTextOutputFormat()
    {
        GateAudioProcessor processor;
        const auto parameters = getThresholdParameters (processor);
        expect (! parameters.empty());
        if (parameters.empty())
            return;

        juce::AudioProcessorParameter& hostView = *parameters.front();
        expectEquals (hostView.getText (parameters.front()->convertTo0to1 (-30.0f), 0), juce::String ("-30.0 dB"));
        expectEquals (hostView.getText (parameters.front()->convertTo0to1 (-6.5f), 0), juce::String ("-6.5 dB"));
    }

    void testParameterCount()
    {
        GateAudioProcessor processor;
        const auto thresholdParameters = getThresholdParameters (processor);
        const int total = processor.getParameters().size();

        expectEquals (static_cast<int> (thresholdParameters.size()), 31,
                      "Gate should expose exactly the 31 ISO 1/3-octave threshold points (got "
                          + juce::String (static_cast<int> (thresholdParameters.size())) + ")");
        expect (total < 64,
                "Gate should not flood the host with unused parameters (exposes " + juce::String (total) + ")");
        expect (processor.getAPVTS().getParameter ("bypass_lowest_band") != nullptr,
                "bypass_lowest_band should be kept");
    }

    void testFirstParameterActsOnLowestBandInEveryTopology()
    {
        for (const auto& [label, destinations] : getTopologies())
        {
            auto processor = createPreparedProcessor();
            expect (processor->applyTopologyCandidateForTest (destinations), label + ": topology should be accepted");

            const auto parameters = getThresholdParameters (*processor);
            expect (! parameters.empty(), label + ": threshold parameters should exist");
            if (parameters.empty())
                continue;

            for (auto* parameter : parameters)
                setParameterDb (*parameter, -100.0f);
            setParameterDb (*parameters.front(), -20.0f);

            processOneBlock (*processor);

            const auto thresholdsDb = readBandThresholdsDb (*processor, static_cast<int> (destinations.size()));
            const auto loudest = static_cast<std::size_t> (std::max_element (thresholdsDb.begin(), thresholdsDb.end())
                                                           - thresholdsDb.begin());

            const auto& path = destinations[loudest];
            const bool isLowestBand = std::all_of (path.begin(), path.end(), [] (char c) { return c == 'L'; });
            expect (isLowestBand,
                    label + ": the lowest-frequency parameter (" + parameters.front()->paramID
                        + ") should act on the lowest band, but it acted on band \"" + juce::String (path) + "\"");
        }
    }

    void testOneKilohertzPointControlsItsBandInEveryTopology()
    {
        for (const auto& [label, destinations] : getTopologies())
        {
            auto processor = createPreparedProcessor();
            expect (processor->applyTopologyCandidateForTest (destinations), label + ": topology should be accepted");

            auto* oneKilohertz = findFloatParameter (*processor, "threshold_1khz");
            expect (oneKilohertz != nullptr, label + ": parameter threshold_1khz should exist");
            if (oneKilohertz == nullptr)
                continue;

            for (auto* parameter : getThresholdParameters (*processor))
                setParameterDb (*parameter, -60.0f);
            setParameterDb (*oneKilohertz, -20.0f);

            processOneBlock (*processor);

            const auto thresholdsDb = readBandThresholdsDb (*processor, static_cast<int> (destinations.size()));

            for (std::size_t band = 0; band < destinations.size(); ++band)
            {
                const auto [lo, hi] = bandRange (destinations[band]);
                const juce::String where = label + ", band \"" + juce::String (destinations[band]) + "\" ("
                                         + juce::String (lo, 0) + "-" + juce::String (hi, 0) + " Hz)";

                if (lo < 1000.0f && 1000.0f < hi)
                {
                    expectGreaterThan (thresholdsDb[band], -60.0 + 0.1);
                    expect (thresholdsDb[band] <= -20.0 + 1.0e-9, where + ": must not exceed the 1 kHz point value");
                }
                else if (hi <= 800.0f || lo >= 1250.0f)
                {
                    expectWithinAbsoluteError (thresholdsDb[band], -60.0, 1.0e-6,
                                               where + ": is outside the neighbouring points and must be unaffected");
                }
            }
        }
    }
};

static GateParameterDesignTests gateParameterDesignTests;
