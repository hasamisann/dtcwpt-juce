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

    /** Returns Hanken Grotesk font for text buttons. */
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    /** Returns Hanken Grotesk font for labels. */
    juce::Font getLabelFont (juce::Label&) override;

    /** Returns Hanken Grotesk font for slider text boxes. */
    juce::Font getSliderPopupFont (juce::Slider&) override;

private:
    //==============================================================================
    // Visual constants
    //==============================================================================

    /** Background track colour: dark grey. */
    static constexpr juce::uint32 kTrackColour  = 0xFF38393B;

    /** Foreground fill colour: accent blue. */
    static constexpr juce::uint32 kFillColour   = 0xFF6CB1FF;

    /** Centre knob body colour: medium grey. */
    static constexpr juce::uint32 kKnobColour   = 0xFFB7BCC1;

    /** Indicator line colour. */
    static constexpr juce::uint32 kIndicatorColour = 0xFF202026;

    /** Arc line width as a fraction of the radius. */
    static constexpr float kArcWidthFraction = 0.12f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WiperLookAndFeel)
};
