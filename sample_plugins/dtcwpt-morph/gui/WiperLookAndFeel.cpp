/**
 * @file WiperLookAndFeel.cpp
 * @brief Custom LookAndFeel for wiper-style arc rotary knobs.
 */

#include "WiperLookAndFeel.h"
#include "FontHelper.h"

//==============================================================================
// Constructor
//==============================================================================

WiperLookAndFeel::WiperLookAndFeel()
{
    // Apply dark colour scheme as the base
    setColourScheme (getDarkColourScheme());

    // Customise slider colours
    // Thumb: transparent (we draw our own arc, no thumb needed)
    setColour (juce::Slider::thumbColourId,
               juce::Colour (0x00000000));

    // Track background: dark grey (matches kTrackColour)
    setColour (juce::Slider::trackColourId,
               juce::Colour (kTrackColour));

    // Rotary fill: accent blue (matches kFillColour)
    setColour (juce::Slider::rotarySliderFillColourId,
               juce::Colour (kFillColour));

    // Rotary outline: slightly lighter grey for track outline
    setColour (juce::Slider::rotarySliderOutlineColourId,
               juce::Colour (kTrackColour));
}

//==============================================================================
// drawRotarySlider
//==============================================================================

void WiperLookAndFeel::drawRotarySlider (juce::Graphics& g,
                                          int   x,
                                          int   y,
                                          int   width,
                                          int   height,
                                          float sliderPosProportional,
                                          float rotaryStartAngle,
                                          float rotaryEndAngle,
                                          juce::Slider& /*slider*/)
{
    // -----------------------------------------------------------------------
    // Geometry
    // -----------------------------------------------------------------------

    const float centreX  = static_cast<float> (x) + static_cast<float> (width)  * 0.5f;
    const float centreY  = static_cast<float> (y) + static_cast<float> (height) * 0.5f;
    const float radius   = juce::jmin (static_cast<float> (width),
                                       static_cast<float> (height)) * 0.5f - 2.0f;
    const float arcWidth = radius * kArcWidthFraction * 2.0f;  // stroke width (both sides)
    const float strokeThickness = radius * kArcWidthFraction;
    
    // Current value angle (clamped to [startAngle, endAngle])
    const float toAngle  = rotaryStartAngle
                           + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    // Arc bounding rectangle (centred, inset by half stroke so it perfectly fits)
    const float arcRadius = radius - strokeThickness * 0.5f;
    const juce::Rectangle<float> arcBounds (centreX - arcRadius,
                                             centreY - arcRadius,
                                             arcRadius * 2.0f,
                                             arcRadius * 2.0f);

    // -----------------------------------------------------------------------
    // 1. Background track arc (dark grey, full range)
    // -----------------------------------------------------------------------

    juce::Path trackArc;
    trackArc.addArc (arcBounds.getX(),
                     arcBounds.getY(),
                     arcBounds.getWidth(),
                     arcBounds.getHeight(),
                     rotaryStartAngle,
                     rotaryEndAngle,
                     true);

    g.setColour (juce::Colour (kTrackColour));
    g.strokePath (trackArc,
                  juce::PathStrokeType (strokeThickness,
                                        juce::PathStrokeType::curved,
                                        juce::PathStrokeType::butt));

    // -----------------------------------------------------------------------
    // 2. Filled value arc (accent blue, start → current angle)
    // -----------------------------------------------------------------------

    if (toAngle > rotaryStartAngle)
    {
        juce::Path fillArc;
        fillArc.addArc (arcBounds.getX(),
                        arcBounds.getY(),
                        arcBounds.getWidth(),
                        arcBounds.getHeight(),
                        rotaryStartAngle,
                        toAngle,
                        true);

        g.setColour (juce::Colour (kFillColour));
        g.strokePath (fillArc,
                      juce::PathStrokeType (strokeThickness,
                                            juce::PathStrokeType::curved,
                                            juce::PathStrokeType::butt));
    }

    // -----------------------------------------------------------------------
    // 3. Centre knob body circle
    // -----------------------------------------------------------------------

    const float knobRadius = radius - strokeThickness;
    g.setColour (juce::Colour (kKnobColour));
    g.fillEllipse (centreX - knobRadius,
                   centreY - knobRadius,
                   knobRadius * 2.0f,
                   knobRadius * 2.0f);

    // -----------------------------------------------------------------------
    // 4. Indicator line at current value
    // -----------------------------------------------------------------------

    g.setColour (juce::Colour (kIndicatorColour));
    juce::Path indicator;
    const float indStart = knobRadius * 0.45f;
    const float indEnd   = knobRadius;

    const float angleOffset = toAngle - juce::MathConstants<float>::halfPi;
    indicator.startNewSubPath (centreX + indStart * std::cos (angleOffset),
                               centreY + indStart * std::sin (angleOffset));
    indicator.lineTo          (centreX + indEnd   * std::cos (angleOffset),
                               centreY + indEnd   * std::sin (angleOffset));

    const float indThickness = knobRadius * 0.16f;
    g.strokePath (indicator, juce::PathStrokeType (indThickness, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
}

juce::Font WiperLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return FontHelper::getFont (static_cast<float> (buttonHeight) * 0.6f);
}

juce::Font WiperLookAndFeel::getLabelFont (juce::Label&)
{
    return FontHelper::getFont (13.0f);
}

juce::Font WiperLookAndFeel::getSliderPopupFont (juce::Slider&)
{
    return FontHelper::getFont (13.0f);
}
