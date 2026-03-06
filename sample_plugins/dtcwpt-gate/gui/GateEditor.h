#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../GateAudioProcessor.h"
#include "GateMainView.h"
#include "TopologyEditorView.h"
#include "AboutView.h"

/**
 * @brief The main AudioProcessorEditor for the DT-CWPT Gate plugin.
 *
 * It manages the navigation between three distinct views:
 *  - Main View (header, spectrum analyzer, graphic EQ)
 *  - Settings View (topology editor)
 *  - About View
 *
 * Only one view is visible at a time. The editor has a fixed size.
 */
class GateEditor : public juce::AudioProcessorEditor
{
public:
    explicit GateEditor (GateAudioProcessor& p);
    ~GateEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void showMain();
    void showSettings();
    void showAbout();

    // This reference is provided as a quick way for your editor to
    // access the processor object that created it.
    GateAudioProcessor& processor_;

    const double constructionSampleRate_;

    // Views
    GateMainView mainView_;
    TopologyEditorView topoView_;
    AboutView aboutView_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GateEditor)
};
