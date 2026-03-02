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
 * Parameter synchronisation uses a polling-based approach (ParameterPoller, ~30 Hz)
 * rather than JUCE SliderAttachment / ButtonAttachment.  This avoids registering
 * AudioProcessorParameter::Listener callbacks that would call triggerAsyncUpdate()
 * — and therefore perform a heap allocation — on the audio thread when the host
 * automates parameters, which was the root cause of STATUS_HEAP_CORRUPTION under
 * pluginval's "Open editor whilst processing" test.
 *
 * View-switching callbacks (settings / about) are provided by MorphEditor via
 * setSettingsCallback() and setAboutCallback().
 */

#include "SpectrumAnalyzerWidget.h"
#include "WiperLookAndFeel.h"

#include "MorphAudioProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <vector>

/**
 * @brief Main layout component (header bar, spectrum, knobs, bypass checkboxes).
 *
 * Constructed with a reference to MorphAudioProcessor. Creates all child
 * components, wires knobs/buttons to APVTS parameters via polling, and manages
 * layout.
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
     * Stops the parameter poller and clears LookAndFeel pointers from all
     * sliders to avoid dangling references.
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
    // Polling-based parameter synchronisation
    //
    // Replaces SliderAttachment / ButtonAttachment to eliminate the audio-thread
    // triggerAsyncUpdate() -> heap allocation path (root cause of heap corruption).
    //==============================================================================

    /**
     * @brief Polls APVTS raw parameter values at 30 Hz and updates the UI.
     *
     * Only reads std::atomic<float>* values (relaxed load) on the message thread.
     * No AudioProcessorParameter::Listener is registered, so there is no
     * audio-thread callback path.
     */
    class ParameterPoller : public juce::Timer
    {
    public:
        /** Entry for a slider that mirrors a raw parameter value. */
        struct SliderEntry
        {
            std::atomic<float>* raw;       ///< Pointer to APVTS raw parameter atom.
            juce::Slider*       slider;    ///< The slider to keep in sync.
            float               lastValue; ///< Last observed value (avoid redundant setValue).
        };

        /** Entry for a toggle button that mirrors a raw parameter value. */
        struct ButtonEntry
        {
            std::atomic<float>*  raw;      ///< Pointer to APVTS raw parameter atom.
            juce::ToggleButton*  button;   ///< The button to keep in sync.
            float                lastValue;///< Last observed value.
        };

        /**
         * @brief Registers a slider to be updated from @p raw at each poll tick.
         *
         * @param raw     Pointer returned by APVTS::getRawParameterValue().
         * @param slider  Slider widget to update (must outlive the poller).
         */
        void addSlider (std::atomic<float>* raw, juce::Slider* slider)
        {
            sliders_.push_back ({ raw, slider, -1.0e9f });
        }

        /**
         * @brief Registers a toggle button to be updated from @p raw at each poll tick.
         *
         * @param raw     Pointer returned by APVTS::getRawParameterValue().
         * @param button  Button widget to update (must outlive the poller).
         */
        void addButton (std::atomic<float>* raw, juce::ToggleButton* button)
        {
            buttons_.push_back ({ raw, button, -1.0e9f });
        }

        /** Called on the message thread at ~30 Hz. Pushes new values to UI widgets. */
        void timerCallback() override
        {
            for (auto& e : sliders_)
            {
                const float v = e.raw->load (std::memory_order_relaxed);
                if (v != e.lastValue)
                {
                    e.lastValue = v;
                    e.slider->setValue (static_cast<double> (v),
                                        juce::dontSendNotification);
                }
            }
            for (auto& e : buttons_)
            {
                const float v = e.raw->load (std::memory_order_relaxed);
                if (v != e.lastValue)
                {
                    e.lastValue = v;
                    e.button->setToggleState (v >= 0.5f, juce::dontSendNotification);
                }
            }
        }

    private:
        std::vector<SliderEntry> sliders_;
        std::vector<ButtonEntry> buttons_;
    };

    /** Reference to the processor's APVTS (for write-back lambdas and initial setup). */
    juce::AudioProcessorValueTreeState& apvts_;

    /** Polls raw parameter values and pushes them to UI widgets at ~30 Hz. */
    ParameterPoller poller_;

    //==============================================================================
    // Navigation callbacks
    //==============================================================================

    /** Callback for Settings (⚙) button click. */
    std::function<void()> onSettings_;

    /** Callback for About (ℹ) button click. */
    std::function<void()> onAbout_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainView)
};
