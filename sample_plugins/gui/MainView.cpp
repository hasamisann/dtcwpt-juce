/**
 * @file MainView.cpp
 * @brief Implementation of the main plugin layout component.
 */

#include "MainView.h"

#include "MorphAudioProcessor.h"

//==============================================================================
// Colour constants
//==============================================================================

namespace
{
    /** Dark background colour for the main view. */
    constexpr juce::uint32 kBgColour    = 0xFF1A1A2E;

    /** Header bar background colour. */
    constexpr juce::uint32 kHeaderBg    = 0xFF16213E;

    /** Header bar title text colour. */
    constexpr juce::uint32 kTitleColour = 0xFFDDDDDD;

    /** Knob label text colour. */
    constexpr juce::uint32 kLabelColour = 0xFF999999;

} // namespace

//==============================================================================
// Constructor
//==============================================================================

MainView::MainView (MorphAudioProcessor& processor)
    : spectrumWidget_ (processor.getSpectrumBridge())
    , settingsButton_ ("Settings")
    , aboutButton_    ("About")
    , bypassLowButton_  ("Bypass Low")
    , bypassHighButton_ ("Bypass High")
{
    // -----------------------------------------------------------------------
    // Header bar
    // -----------------------------------------------------------------------
    titleLabel_.setText ("DT-CWPT Morph", juce::dontSendNotification);
    titleLabel_.setFont (juce::Font (juce::FontOptions().withHeight (16.0f).withStyle ("Bold")));
    titleLabel_.setColour (juce::Label::textColourId, juce::Colour (kTitleColour));
    titleLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel_);

    settingsButton_.setButtonText (juce::CharPointer_UTF8 ("\xe2\x9a\x99"));  // ⚙ (U+2699)
    settingsButton_.onClick = [this] { if (onSettings_) onSettings_(); };
    addAndMakeVisible (settingsButton_);

    aboutButton_.setButtonText (juce::CharPointer_UTF8 ("\xe2\x84\xb9"));     // ℹ (U+2139)
    aboutButton_.onClick = [this] { if (onAbout_) onAbout_(); };
    addAndMakeVisible (aboutButton_);

    // -----------------------------------------------------------------------
    // Spectrum analyzer widget
    // -----------------------------------------------------------------------
    addAndMakeVisible (spectrumWidget_);

    // -----------------------------------------------------------------------
    // Magnitude knob
    // -----------------------------------------------------------------------
    magnitudeKnob_.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    magnitudeKnob_.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 18);
    magnitudeKnob_.setLookAndFeel (&wiperLnf_);
    addAndMakeVisible (magnitudeKnob_);

    magnitudeLabel_.setText ("Magnitude", juce::dontSendNotification);
    magnitudeLabel_.setFont (juce::Font (juce::FontOptions().withHeight (11.0f)));
    magnitudeLabel_.setColour (juce::Label::textColourId, juce::Colour (kLabelColour));
    magnitudeLabel_.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (magnitudeLabel_);

    // -----------------------------------------------------------------------
    // Phase knob
    // -----------------------------------------------------------------------
    phaseKnob_.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    phaseKnob_.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 18);
    phaseKnob_.setLookAndFeel (&wiperLnf_);
    addAndMakeVisible (phaseKnob_);

    phaseLabel_.setText ("Phase", juce::dontSendNotification);
    phaseLabel_.setFont (juce::Font (juce::FontOptions().withHeight (11.0f)));
    phaseLabel_.setColour (juce::Label::textColourId, juce::Colour (kLabelColour));
    phaseLabel_.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (phaseLabel_);

    // -----------------------------------------------------------------------
    // Threshold knob
    // -----------------------------------------------------------------------
    thresholdKnob_.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    thresholdKnob_.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 18);
    thresholdKnob_.setLookAndFeel (&wiperLnf_);
    addAndMakeVisible (thresholdKnob_);

    thresholdLabel_.setText ("Threshold", juce::dontSendNotification);
    thresholdLabel_.setFont (juce::Font (juce::FontOptions().withHeight (11.0f)));
    thresholdLabel_.setColour (juce::Label::textColourId, juce::Colour (kLabelColour));
    thresholdLabel_.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (thresholdLabel_);

    // -----------------------------------------------------------------------
    // Bypass toggle buttons
    // -----------------------------------------------------------------------
    addAndMakeVisible (bypassLowButton_);
    addAndMakeVisible (bypassHighButton_);

    // -----------------------------------------------------------------------
    // APVTS attachments (create after adding sliders/buttons to component tree)
    // -----------------------------------------------------------------------
    auto& apvts = processor.getAPVTS();

    magnitudeAttachment_  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        apvts, ParamID::Magnitude,  magnitudeKnob_);

    phaseAttachment_      = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        apvts, ParamID::Phase,      phaseKnob_);

    thresholdAttachment_  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        apvts, ParamID::Threshold,  thresholdKnob_);

    bypassLowAttachment_  = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        apvts, ParamID::BypassLow,  bypassLowButton_);

    bypassHighAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        apvts, ParamID::BypassHigh, bypassHighButton_);
}

//==============================================================================
// Destructor
//==============================================================================

MainView::~MainView()
{
    // Detach APVTS attachments before clearing LookAndFeel pointers so the
    // attachments do not try to use the slider after it is in a destructed state.
    magnitudeAttachment_.reset();
    phaseAttachment_.reset();
    thresholdAttachment_.reset();
    bypassLowAttachment_.reset();
    bypassHighAttachment_.reset();

    // Clear LookAndFeel references to avoid dangling pointer on destruction.
    magnitudeKnob_.setLookAndFeel (nullptr);
    phaseKnob_.setLookAndFeel     (nullptr);
    thresholdKnob_.setLookAndFeel (nullptr);
}

//==============================================================================
// Navigation callbacks
//==============================================================================

void MainView::setSettingsCallback (std::function<void()> cb)
{
    onSettings_ = std::move (cb);
}

void MainView::setAboutCallback (std::function<void()> cb)
{
    onAbout_ = std::move (cb);
}

//==============================================================================
// Paint
//==============================================================================

void MainView::paint (juce::Graphics& g)
{
    // Background
    g.fillAll (juce::Colour (kBgColour));

    // Header bar background
    g.setColour (juce::Colour (kHeaderBg));
    g.fillRect (0, 0, getWidth(), kHeaderHeight);
}

//==============================================================================
// Resized
//==============================================================================

void MainView::resized()
{
    const int w = getWidth();
    const int h = getHeight();

    // -----------------------------------------------------------------------
    // Header bar (top, full width, kHeaderHeight px)
    // -----------------------------------------------------------------------
    constexpr int kNavButtonWidth = 36;
    constexpr int kNavButtonMargin = 4;

    // About button — far right
    aboutButton_.setBounds (w - kNavButtonWidth - kNavButtonMargin,
                            kNavButtonMargin,
                            kNavButtonWidth,
                            kHeaderHeight - 2 * kNavButtonMargin);

    // Settings button — left of About
    settingsButton_.setBounds (w - 2 * (kNavButtonWidth + kNavButtonMargin),
                               kNavButtonMargin,
                               kNavButtonWidth,
                               kHeaderHeight - 2 * kNavButtonMargin);

    // Title — occupies the rest of the header
    titleLabel_.setBounds (8, 0, w - 2 * (kNavButtonWidth + kNavButtonMargin) - 12, kHeaderHeight);

    // -----------------------------------------------------------------------
    // Bottom section heights
    // -----------------------------------------------------------------------
    constexpr int kLabelHeight = 16;
    const int bottomSectionHeight = kKnobRowHeight + kLabelHeight + kCheckboxRowHeight + kBottomMargin;

    // -----------------------------------------------------------------------
    // Spectrum widget — central area between header and bottom section
    // -----------------------------------------------------------------------
    spectrumWidget_.setBounds (0, kHeaderHeight, w, h - kHeaderHeight - bottomSectionHeight);

    // -----------------------------------------------------------------------
    // Knob row — three equal-width sections
    // -----------------------------------------------------------------------
    const int knobRowY = h - bottomSectionHeight;
    const int colWidth = w / 3;

    magnitudeKnob_.setBounds (0,            knobRowY, colWidth, kKnobRowHeight);
    phaseKnob_.setBounds     (colWidth,     knobRowY, colWidth, kKnobRowHeight);
    thresholdKnob_.setBounds (colWidth * 2, knobRowY, colWidth, kKnobRowHeight);

    // Labels below knobs
    const int labelY = knobRowY + kKnobRowHeight;
    magnitudeLabel_.setBounds (0,            labelY, colWidth, kLabelHeight);
    phaseLabel_.setBounds     (colWidth,     labelY, colWidth, kLabelHeight);
    thresholdLabel_.setBounds (colWidth * 2, labelY, colWidth, kLabelHeight);

    // -----------------------------------------------------------------------
    // Bypass checkboxes row — below knob labels
    // -----------------------------------------------------------------------
    const int checkY = labelY + kLabelHeight;
    const int checkWidth = w / 2;
    bypassLowButton_.setBounds  (0,          checkY, checkWidth, kCheckboxRowHeight);
    bypassHighButton_.setBounds (checkWidth, checkY, checkWidth, kCheckboxRowHeight);
}
