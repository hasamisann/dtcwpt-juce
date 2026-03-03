#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../GateAudioProcessor.h"
#include "SpectrumAnalyzerWidget.h"
#include "GraphicEqWidget.h"
#include <functional>

/**
 * @brief Main View for the DT-CWPT Gate plugin.
 *
 * Lays out the header bar (with title and ⚙/ℹ buttons), the 
 * SpectrumAnalyzerWidget, and the GraphicEqWidget.
 * Exposes callbacks for the Editor to handle view switching.
 */
class GateMainView : public juce::Component
{
public:
    GateMainView (GateAudioProcessor& processor, double sampleRate);
    ~GateMainView() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    // Callbacks wired up by GateEditor
    std::function<void()> onSettingsClicked;
    std::function<void()> onAboutClicked;

private:
    GateAudioProcessor& processor_;

    // UI Components (order is important: children must be destroyed before dependencies)
    juce::Label titleLabel_;
    juce::TextButton settingsButton_;
    juce::TextButton aboutButton_;

    SpectrumAnalyzerWidget spectrumWidget_;
    GraphicEqWidget graphicEqWidget_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GateMainView)
};
