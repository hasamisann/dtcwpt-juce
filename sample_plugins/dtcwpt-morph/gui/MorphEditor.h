#pragma once

/**
 * @file MorphEditor.h
 * @brief Root AudioProcessorEditor for the DT-CWPT Morph plugin.
 *
 * MorphEditor is the top-level juce::AudioProcessorEditor. It implements a
 * view-stack pattern that switches between three sub-views:
 *  - MainView        — default view (knobs, spectrum, bypass checkboxes)
 *  - TopologyEditorView — wavelet tree editor (accessible via Settings ⚙ button)
 *  - AboutView       — static info page (accessible via About ℹ button)
 *
 * Only one view is visible at a time. View switching is managed by the
 * showMain(), showTopology(), and showAbout() methods, which are wired as
 * callbacks into the sub-views.
 *
 * The editor is resizable with a fixed 7:10 aspect ratio. Minimum size is
 * 280×400 pixels; maximum size is 560×800 pixels (2× scale).
 */

#include "MainView.h"
#include "TopologyEditorView.h"
#include "AboutView.h"

#include "MorphAudioProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

/**
 * @brief Root editor component for the DT-CWPT Morph plugin.
 *
 * Owns the three sub-views and manages their visibility based on the current
 * view state. Constructed with a reference to the owning MorphAudioProcessor.
 */
class MorphEditor : public juce::AudioProcessorEditor
{
public:
    //==============================================================================
    // Constructor / Destructor
    //==============================================================================

    /**
     * @brief Constructs the editor.
     *
     * Creates all three sub-views, wires navigation callbacks, shows MainView,
     * and configures the editor to be resizable with a fixed 7:10 aspect ratio
     * (min: 280×400, max: 560×800).
     *
     * @param processor  Reference to the owning MorphAudioProcessor.
     *                   Passed to sub-views that need APVTS or bridge access.
     */
    explicit MorphEditor (MorphAudioProcessor& processor);

    /** Destructor. Removes child components before member sub-views are destroyed. */
    ~MorphEditor() override;

    //==============================================================================
    // juce::Component overrides
    //==============================================================================

    /** Sets the bounds of the current visible sub-view to fill the entire editor. */
    void resized() override;

    //==============================================================================
    // View switching
    //==============================================================================

    /** Switches to the main view (hides topology and about views). */
    void showMain();

    /** Switches to the topology editor view (hides main and about views). */
    void showTopology();

    /** Switches to the about view (hides main and topology views). */
    void showAbout();

private:
    //==============================================================================
    // Fixed editor dimensions
    //==============================================================================

    /** Editor width in pixels. */
    static constexpr int kEditorWidth  = 360;

    /** Editor height in pixels. */
    static constexpr int kEditorHeight = 512;

    //==============================================================================
    // Sub-views (owned by this editor)
    //==============================================================================

    /** Primary view with knobs, spectrum analyzer, and navigation buttons. */
    MainView mainView_;

    /** Wavelet tree topology editor view. */
    TopologyEditorView topologyView_;

    /** Static About page. */
    AboutView aboutView_;

    //==============================================================================
    // View state
    //==============================================================================

    /**
     * @brief Enumeration of the three possible views.
     */
    enum class ViewState
    {
        Main,
        Topology,
        About
    };

    /** Currently active view (used to route resized() layout). */
    ViewState currentView_ { ViewState::Main };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphEditor)
};
