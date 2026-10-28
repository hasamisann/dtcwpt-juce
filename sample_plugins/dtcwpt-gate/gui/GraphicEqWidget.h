#pragma once

/**
 * @file GraphicEqWidget.h
 * @brief GUI Component for threshold editing and per-band level metering.
 *
 * This widget provides a graphic equalizer-style interface for the noise gate
 * thresholds. The editable values are the 31 ISO 1/3-octave threshold points
 * (see GateThresholdCurve.h), drawn as a curve with a handle per point.
 * Behind the curve, a filled bar per active band shows the threshold the band
 * actually uses (the mean of the curve over the band's frequency range), and a
 * real-time RMS level meter line is overlaid for each band.
 * Points can be painted by clicking and dragging across the widget.
 * A context menu on right-click allows resetting or fully opening/closing
 * the gate around a specific point.
 */

#include "GateGuiFrequencyMapping.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include "../GateAudioProcessor.h"
#include <array>
#include <atomic>
#include <string>
#include <vector>

/**
 * @brief Graphic EQ interactive widget for threshold control and level display.
 *
 * Runs a 30 Hz timer to animate the level meters. Caches parameter pointers
 * from the APVTS to ensure safe, allocation-free access during paint and mouse
 * events. Support for tooltips and per-point right-click context menus.
 */
class GraphicEqWidget : public juce::Component, public juce::Timer
{
public:
    /**
     * @brief Constructs the GraphicEqWidget.
     *
     * @param processor  Reference to the main audio processor (for APVTS, TopologyState, etc.)
     * @param sampleRate The current audio sample rate (used for calculating Nyquist frequency mappings)
     */
    GraphicEqWidget (GateAudioProcessor& processor, float sampleRate);

    ~GraphicEqWidget() override;

    //==============================================================================
    // juce::Component interface
    //==============================================================================

    void paint (juce::Graphics& g) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;
    
    // Support hover updates
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    //==============================================================================
    // juce::Timer interface
    //==============================================================================

    /** 
     * @brief Animates the level meters at 30 Hz.
     */
    void timerCallback() override;

private:
    //==============================================================================
    // Private Helpers
    //==============================================================================

    /** Calculates min/max frequency range for a given band path. */
    std::pair<float, float> getBandFreqRange (const std::string& path) const;

    /** Map frequency (Hz) to X coordinate within current bounds. */
    float freqToX (float freq) const;

    /** Map X coordinate to frequency (Hz). */
    float xToFreq (float x) const;

    /** Map threshold (dB) to Y coordinate within current bounds. */
    float dbToY (float db) const;

    /** Map Y coordinate to threshold (dB). */
    float yToDb (float y) const;

    /** Updates one threshold point from a Y position in widget coordinates. */
    void updateThresholdForPointFromY (int pointIdx, float y);

    /** Applies one drag segment and fills all crossed threshold points. */
    void applyDragSegment (juce::Point<float> from, juce::Point<float> to);

    /** Begin/end gesture lifecycle for touched points. */
    void beginGestureForPoint (int pointIdx);
    void endAllActiveGestures();

    /** Returns X coordinate of a threshold point. */
    float getPointX (int pointIdx) const;

    /** Returns the threshold point nearest to the given X coordinate (log frequency axis). */
    int findPointAtX (float x) const;

    /** Number of threshold points shown for the current Nyquist frequency. */
    int getNumVisiblePoints() const;

    /** Current values of all threshold points in dB. */
    std::array<double, gate_threshold::kNumPoints> readPointsDb() const;

    //==============================================================================
    // Data References
    //==============================================================================

    GateAudioProcessor& processor_;
    float sampleRate_;
    float nyquist_;

    // Cached current destinations for stable drawing (refreshed in paint)
    std::vector<std::string> currentDestinations_;

    //==============================================================================
    // Cached Parameters
    //==============================================================================

    /** Cached raw parameter states for fast drawing. */
    std::array<std::atomic<float>*, gate_threshold::kNumPoints> thresholdRaw_ {};

    /** Cached parameter objects for host notification during interactions. */
    std::array<juce::RangedAudioParameter*, gate_threshold::kNumPoints> thresholdParams_ {};

    /** Determines if the lowest band is visually bypassed. */
    std::atomic<float>* bypassLowestRaw_ {nullptr};

    //==============================================================================
    // Interaction State
    //==============================================================================

    bool isDragging_ = false;
    juce::Point<float> lastMousePos_ {};
    std::array<bool, gate_threshold::kNumPoints> activePointGestures_ {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphicEqWidget)
};
