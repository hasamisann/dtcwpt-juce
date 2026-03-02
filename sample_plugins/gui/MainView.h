#pragma once

/**
 * @file MainView.h
 * @brief Main layout component for the DT-CWPT Morph plugin.
 *
 * MainView contains the plugin's primary GUI:
 *  - Header bar: "DT-CWPT Morph" title, Settings (⚙) and About (ℹ) navigation buttons.
 *  - SpectrumAnalyzerWidget occupying the central area.
 *  - Three rotary knobs (Magnitude, Phase, Threshold) using WiperLookAndFeel.
 *  - Two toggle buttons (Bypass Low, Bypass High).
 *
 * All knobs and buttons are connected to the MorphAudioProcessor's APVTS via
 * SliderAttachment and ButtonAttachment, so host automation is fully supported.
 *
 * View-switching callbacks (settings / about) are provided by MorphEditor via
 * setSettingsCallback() and setAboutCallback().
 */

#include "SpectrumAnalyzerWidget.h"
#include "WiperLookAndFeel.h"

#include "MorphAudioProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

/**
 * @brief Main layout component (header bar, spectrum, knobs, bypass checkboxes).
 *
 * Constructed with a reference to MorphAudioProcessor. Creates all child
 * components, attaches knobs/buttons to APVTS parameters, and manages layout.
 */
class MainView : public juce::Component
{
public:
    //==============================================================================
    // Constructor / Destructor
    //==============================================================================

    /**
     * @brief Constructs the main view.
     *
     * @param processor  Reference to the MorphAudioProcessor that owns the APVTS
     *                   and SpectrumDataBridge. Must outlive this view.
     */
    explicit MainView (MorphAudioProcessor& processor);

    /**
     * @brief Destructor.
     *
     * Clears LookAndFeel pointers from all sliders to avoid dangling references.
     */
    ~MainView() override;

    //==============================================================================
    // Navigation callbacks (set by MorphEditor)
    //==============================================================================

    /**
     * @brief Sets the callback invoked when the user clicks the Settings (⚙) button.
     *
     * @param cb  Callback with signature void().
     */
    void setSettingsCallback (std::function<void()> cb);

    /**
     * @brief Sets the callback invoked when the user clicks the About (ℹ) button.
     *
     * @param cb  Callback with signature void().
     */
    void setAboutCallback (std::function<void()> cb);

    //==============================================================================
    // juce::Component overrides
    //==============================================================================

    /** Fills the background with a dark colour. */
    void paint (juce::Graphics& g) override;

    /** Positions all child components according to the layout specification. */
    void resized() override;

private:
    //==============================================================================
    // Layout constants
    //==============================================================================

    /** Height of the header bar in pixels. */
    static constexpr int kHeaderHeight = 40;

    /** Height of the knob row in pixels. */
    static constexpr int kKnobRowHeight = 80;

    /** Height of the bypass checkbox row in pixels. */
    static constexpr int kCheckboxRowHeight = 28;

    /** Bottom margin below checkbox row. */
    static constexpr int kBottomMargin = 4;

    //==============================================================================
    // Header bar children
    //==============================================================================

    /** Plugin title label: "DT-CWPT Morph". */
    juce::Label titleLabel_;

    /** Settings navigation button: "⚙". */
    juce::TextButton settingsButton_;

    /** About navigation button: "ℹ". */
    juce::TextButton aboutButton_;

    //==============================================================================
    // Spectrum analyzer
    //==============================================================================

    /** Real-time spectrum analyzer widget (polls SpectrumDataBridge at ~30 Hz). */
    SpectrumAnalyzerWidget spectrumWidget_;

    //==============================================================================
    // Rotary knobs
    //==============================================================================

    /** Custom LookAndFeel for all three rotary knobs. */
    WiperLookAndFeel wiperLnf_;

    /** Magnitude morph knob [0, 1]. */
    juce::Slider magnitudeKnob_;

    /** Phase morph knob [0, 1]. */
    juce::Slider phaseKnob_;

    /** Threshold gate knob [-60, 0 dB]. */
    juce::Slider thresholdKnob_;

    /** Label below the magnitude knob. */
    juce::Label magnitudeLabel_;

    /** Label below the phase knob. */
    juce::Label phaseLabel_;

    /** Label below the threshold knob. */
    juce::Label thresholdLabel_;

    //==============================================================================
    // Bypass toggle buttons
    //==============================================================================

    /** Bypass Low toggle (excludes lowest-frequency band from morphing). */
    juce::ToggleButton bypassLowButton_;

    /** Bypass High toggle (excludes highest-frequency band from morphing). */
    juce::ToggleButton bypassHighButton_;

    //==============================================================================
    // APVTS attachments
    //==============================================================================

    /** Attachment connecting magnitudeKnob_ to the "magnitude" APVTS parameter. */
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> magnitudeAttachment_;

    /** Attachment connecting phaseKnob_ to the "phase" APVTS parameter. */
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> phaseAttachment_;

    /** Attachment connecting thresholdKnob_ to the "threshold" APVTS parameter. */
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> thresholdAttachment_;

    /** Attachment connecting bypassLowButton_ to the "bypassLow" APVTS parameter. */
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassLowAttachment_;

    /** Attachment connecting bypassHighButton_ to the "bypassHigh" APVTS parameter. */
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassHighAttachment_;

    //==============================================================================
    // Navigation callbacks
    //==============================================================================

    /** Callback for Settings (⚙) button click. */
    std::function<void()> onSettings_;

    /** Callback for About (ℹ) button click. */
    std::function<void()> onAbout_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainView)
};
