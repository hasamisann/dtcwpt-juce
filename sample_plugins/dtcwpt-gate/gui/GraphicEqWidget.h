#pragma once

/**
 * @file GraphicEqWidget.h
 * @brief GUI Component for per-band threshold editing and level metering.
 *
 * This widget provides a graphic equalizer-style interface for setting the
 * noise gate thresholds of the DT-CWPT subbands. It displays a filled bar
 * for each active band, where the top of the bar represents the threshold.
 * It also overlays a real-time RMS level meter line for each band.
 * Thresholds can be painted by clicking and dragging across the widget.
 * A context menu on right-click allows resetting or fully opening/closing 
 * the gate for a specific band.
 */

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
 * events. Support for tooltips and per-band right-click context menus.
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

    /** Updates the parameter associated with the band at the given mouse event. */
    void updateThresholdFromMouse (const juce::MouseEvent& e);
    
    /** Returns the index of the band that covers the given X coordinate. */
    int findBandAtX (float x) const;

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
    std::array<std::atomic<float>*, GateBandProcessor::kMaxBands> thresholdRaw_ {};

    /** Cached parameter objects for host notification during interactions. */
    std::array<juce::RangedAudioParameter*, GateBandProcessor::kMaxBands> thresholdParams_ {};

    /** Determines if the lowest band is visually bypassed. */
    std::atomic<float>* bypassLowestRaw_ {nullptr};

    //==============================================================================
    // Interaction State
    //==============================================================================

    bool isDragging_ = false;
    juce::Point<float> lastMousePos_ {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphicEqWidget)
};
