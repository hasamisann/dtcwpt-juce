/**
 * @file StateVersioningTests.cpp
 * @brief Regression tests for state flush, type validation and versioning
 *        of getStateInformation / setStateInformation (Morph and Gate).
 */

#include "GateAudioProcessor.h"
#include "MorphAudioProcessor.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cmath>
#include <limits>
#include <memory>

class StateVersioningTests : public juce::UnitTest
{
public:
    StateVersioningTests() : juce::UnitTest ("StateVersioningTests") {}

    void runTest() override
    {
        beginTest ("Morph saves parameter changes made right before getStateInformation");
        testSavedStateReflectsLatestParameterChange<MorphAudioProcessor> ("magnitude", 0.9f);

        beginTest ("Gate saves parameter changes made right before getStateInformation");
        testSavedStateReflectsLatestParameterChange<GateAudioProcessor> ("bypass_lowest_band", 1.0f);

        beginTest ("Gate saves threshold changes made right before getStateInformation");
        testSavedStateReflectsLatestParameterChange<GateAudioProcessor> ("threshold_20hz", 0.75f);

        beginTest ("Morph saved state carries the current stateVersion");
        testSavedStateCarriesStateVersion<MorphAudioProcessor>();

        beginTest ("Gate saved state carries the current stateVersion");
        testSavedStateCarriesStateVersion<GateAudioProcessor>();

        beginTest ("Morph ignores state of a different type");
        testForeignTypeStateIsIgnored<MorphAudioProcessor> ("magnitude");

        beginTest ("Gate ignores state of a different type");
        testForeignTypeStateIsIgnored<GateAudioProcessor> ("bypass_lowest_band");

        beginTest ("Morph loads legacy state without stateVersion and re-saves it versioned");
        testLegacyStateLoadsAndResavesVersioned<MorphAudioProcessor> ("MorphParams", "magnitude", 0.5f);

        beginTest ("Gate loads legacy state without stateVersion and re-saves it versioned");
        testLegacyStateLoadsAndResavesVersioned<GateAudioProcessor> ("GateParams", "bypass_lowest_band", 1.0f);

        beginTest ("Morph loads state with an unknown future stateVersion on a best-effort basis");
        testFutureVersionStateLoadsBestEffort<MorphAudioProcessor> ("MorphParams", "magnitude", 0.5f);

        beginTest ("Gate loads state with an unknown future stateVersion on a best-effort basis");
        testFutureVersionStateLoadsBestEffort<GateAudioProcessor> ("GateParams", "bypass_lowest_band", 1.0f);
    }

private:
    static constexpr const char* kVersionKey = "stateVersion";

    template <typename Processor>
    static juce::ValueTree readSavedState (Processor& processor)
    {
        juce::MemoryBlock block;
        processor.getStateInformation (block);
        return juce::ValueTree::readFromData (block.getData(), block.getSize());
    }

    static float savedParameterValue (const juce::ValueTree& state, const juce::String& parameterId)
    {
        for (const auto& child : state)
            if (child.getProperty ("id").toString() == parameterId)
                return static_cast<float> (static_cast<double> (child.getProperty ("value")));

        return std::numeric_limits<float>::quiet_NaN();
    }

    static juce::MemoryBlock makeStateData (const juce::String& type,
                                            const juce::String& parameterId,
                                            float value,
                                            const juce::var& version)
    {
        juce::ValueTree state { juce::Identifier (type) };
        if (! version.isVoid())
            state.setProperty (kVersionKey, version, nullptr);

        juce::ValueTree param { "PARAM" };
        param.setProperty ("id", parameterId, nullptr);
        param.setProperty ("value", value, nullptr);
        state.appendChild (param, nullptr);

        juce::MemoryBlock block;
        juce::MemoryOutputStream stream (block, true);
        state.writeToStream (stream);
        return block;
    }

    template <typename Processor>
    void testSavedStateReflectsLatestParameterChange (const juce::String& parameterId, float normalised)
    {
        Processor processor;
        auto* parameter = processor.getAPVTS().getParameter (parameterId);
        expect (parameter != nullptr, "Parameter must exist");
        if (parameter == nullptr)
            return;

        parameter->setValueNotifyingHost (normalised);
        const float expected = parameter->convertFrom0to1 (normalised);

        // No message loop runs here: the APVTS timer has not flushed the value.
        const float saved = savedParameterValue (readSavedState (processor), parameterId);
        expect (std::abs (saved - expected) < 1.0e-4f,
                "Saved value must match the live parameter (saved=" + juce::String (saved)
                    + " live=" + juce::String (expected) + ")");
    }

    template <typename Processor>
    void testSavedStateCarriesStateVersion()
    {
        Processor processor;
        const auto saved = readSavedState (processor);

        expect (saved.isValid(), "Saved state must be readable");
        expect (saved.hasProperty (kVersionKey), "Saved state root must carry stateVersion");
        expectEquals (static_cast<int> (saved.getProperty (kVersionKey, -1)), 1,
                      "Current stateVersion must be 1");
    }

    template <typename Processor>
    void testForeignTypeStateIsIgnored (const juce::String& parameterId)
    {
        Processor processor;
        const auto typeBefore = processor.getAPVTS().state.getType();
        const float liveBefore = processor.getAPVTS().getParameter (parameterId)->getValue();

        const auto foreign = makeStateData ("OtherParams", parameterId, 1.0f, 1);
        processor.setStateInformation (foreign.getData(), static_cast<int> (foreign.getSize()));

        expect (processor.getAPVTS().state.getType() == typeBefore,
                "State of a foreign type must not replace the APVTS tree");
        expectEquals (processor.getAPVTS().getParameter (parameterId)->getValue(), liveBefore,
                      "State of a foreign type must not change parameters");
    }

    template <typename Processor>
    void testLegacyStateLoadsAndResavesVersioned (const juce::String& type,
                                                  const juce::String& parameterId,
                                                  float value)
    {
        Processor processor;
        const auto legacy = makeStateData (type, parameterId, value, juce::var());
        processor.setStateInformation (legacy.getData(), static_cast<int> (legacy.getSize()));

        auto* parameter = processor.getAPVTS().getParameter (parameterId);
        expect (std::abs (parameter->convertFrom0to1 (parameter->getValue()) - value) < 1.0e-4f,
                "Legacy state (no stateVersion) must still be loaded");

        const auto resaved = readSavedState (processor);
        expectEquals (static_cast<int> (resaved.getProperty (kVersionKey, -1)), 1,
                      "Re-saved legacy state must carry the current stateVersion");
    }

    template <typename Processor>
    void testFutureVersionStateLoadsBestEffort (const juce::String& type,
                                                const juce::String& parameterId,
                                                float value)
    {
        Processor processor;
        const auto future = makeStateData (type, parameterId, value, 99);
        processor.setStateInformation (future.getData(), static_cast<int> (future.getSize()));

        auto* parameter = processor.getAPVTS().getParameter (parameterId);
        expect (std::abs (parameter->convertFrom0to1 (parameter->getValue()) - value) < 1.0e-4f,
                "State with a future stateVersion must still load known parameters");
    }
};

static StateVersioningTests stateVersioningTests;
