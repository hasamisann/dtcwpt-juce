#pragma once

/**
 * @file WiperLookAndFeel.h
 * @brief Custom LookAndFeel for wiper-style arc rotary knobs.
 *
 * WiperLookAndFeel is a juce::LookAndFeel_V4 subclass that overrides
 * drawRotarySlider() to render an arc/wiper-style rotary knob:
 *   - A dark-grey background arc spanning the full rotation range.
 *   - A filled accent-blue foreground arc from the start angle to the current value.
 *   - A small centre circle (knob body).
 *
 * Compatible with any juce::Slider in RotaryVerticalDrag style.
 *
 * Usage:
 * @code
 *   WiperLookAndFeel laf;
 *   mySlider.setLookAndFeel(&laf);
 * @endcode
 */

#include <juce_gui_basics/juce_gui_basics.h>

/**
 * @brief Wiper-arc LookAndFeel for JUCE rotary sliders.
 *
 * Renders a clean, antialiased wiper-arc knob with a single accent colour.
 * The arc design is value-independent in colour — only the sweep angle changes.
 */
class WiperLookAndFeel : public juce::LookAndFeel_V4
{
public:
    //==============================================================================
    // Constructor
    //==============================================================================

    /** Constructs with a dark colour scheme and customised slider colours. */
    WiperLookAndFeel();

    //==============================================================================
    // LookAndFeel overrides
    //==============================================================================

    /**
     * @brief Draws a wiper-arc rotary slider.
     *
     * @param g                     JUCE Graphics context
     * @param x                     Bounding box x
     * @param y                     Bounding box y
     * @param width                 Bounding box width
     * @param height                Bounding box height
     * @param sliderPosProportional Normalised slider value [0, 1]
     * @param rotaryStartAngle      Start angle in radians (12 o'clock ≈ -π * 3/4)
     * @param rotaryEndAngle        End angle in radians
     * @param slider                The owning Slider (unused — single accent colour)
     */
    void drawRotarySlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPosProportional,
                           float rotaryStartAngle,
                           float rotaryEndAngle,
                           juce::Slider& slider) override;

private:
    //==============================================================================
    // Visual constants
    //==============================================================================

    /** Background track colour: dark grey. */
    static constexpr juce::uint32 kTrackColour  = 0xFF3A3A3A;

    /** Foreground fill colour: accent blue. */
    static constexpr juce::uint32 kFillColour   = 0xFF4A9FFF;

    /** Centre knob body colour: medium grey. */
    static constexpr juce::uint32 kKnobColour   = 0xFF555555;

    /** Arc line width as a fraction of the radius. */
    static constexpr float kArcWidthFraction = 0.12f;

    /** Knob body radius as a fraction of the total radius. */
    static constexpr float kKnobRadiusFraction = 0.35f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WiperLookAndFeel)
};
