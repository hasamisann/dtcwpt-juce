#pragma once

/**
 * @file EngineStatusBanner.h
 * @brief Editor overlay that tells the user the DSP engine could not be created.
 *
 * The banner polls a predicate on the message thread (the audio thread only flips an atomic flag)
 * and shows itself only while the predicate reports the engine as unavailable.
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>

class EngineStatusBanner : public juce::Component,
                           private juce::Timer
{
public:
    /** @param isEngineAvailable  Called on the message thread; must be cheap and thread-safe. */
    explicit EngineStatusBanner (std::function<bool()> isEngineAvailable)
        : isEngineAvailable_ (std::move (isEngineAvailable))
    {
        setInterceptsMouseClicks (false, false);
        refresh();
        startTimerHz (4);
    }

    ~EngineStatusBanner() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xE6402020));
        g.fillRoundedRectangle (area, 6.0f);
        g.setColour (juce::Colour (0xFFFFFFFF));
        g.setFont (14.0f);
        g.drawFittedText ("Audio engine unavailable.\nThe plugin is muted. Check available memory\nand reload the plugin or the project.",
                          getLocalBounds().reduced (8), juce::Justification::centred, 3);
    }

private:
    void timerCallback() override { refresh(); }

    void refresh()
    {
        const bool shouldShow = ! isEngineAvailable_();
        if (shouldShow != isVisible())
        {
            setVisible (shouldShow);
            if (shouldShow)
                toFront (false);
        }
    }

    std::function<bool()> isEngineAvailable_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngineStatusBanner)
};
