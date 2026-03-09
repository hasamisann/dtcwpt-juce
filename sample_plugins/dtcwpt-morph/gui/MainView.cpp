/**
 * @file MainView.cpp
 * @brief Implementation of the main plugin layout component.
 */

#include "MainView.h"

#include "FontHelper.h"
#include "MorphAudioProcessor.h"

//==============================================================================
// Colour constants
//==============================================================================

namespace
{
    /** Unified background colour (header + main area). */
    constexpr juce::uint32 kBgColour    = 0xFF202026;

    /** Header bar background colour — same as main background. */
    constexpr juce::uint32 kHeaderBg    = 0xFF202026;

    /** Header bar title text colour. */
    constexpr juce::uint32 kTitleColour = 0xFFDDDDDD;

    /** Knob label text colour. */
    constexpr juce::uint32 kLabelColour = 0xFFF0F0F0;

} // namespace

//==============================================================================
// Constructor
//==============================================================================

MainView::MainView (MorphAudioProcessor& processor)
    : spectrumWidget_ (processor.getSpectrumBridge())
    , apvts_          (processor.getAPVTS())
    , settingsButton_ ("Settings")
    , aboutButton_    ("About")
    , bypassLowButton_  ("Bypass Low")
    , bypassHighButton_ ("Bypass High")
{
    // -----------------------------------------------------------------------
    // Header bar
    // -----------------------------------------------------------------------
    titleLabel_.setText ("DT-CWPT Morph", juce::dontSendNotification);
    auto titleFont = FontHelper::getFont (24.0f, false, 0.05f);
    titleLabel_.setFont (titleFont);
    titleLabel_.setColour (juce::Label::textColourId, juce::Colour (kTitleColour));
    titleLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel_);

    // Bands button (topology/band configuration)
    settingsButton_.setButtonText ("Freq");
    settingsButton_.setLookAndFeel (&buttonLnf_);
    settingsButton_.onClick = [this] { if (onSettings_) onSettings_(); };
    addAndMakeVisible (settingsButton_);

    // About button with lowercase "i"
    aboutButton_.setButtonText ("Info");
    aboutButton_.setLookAndFeel (&buttonLnf_);
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
    magnitudeKnob_.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    magnitudeKnob_.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour(0x00000000));
    magnitudeKnob_.setColour (juce::Slider::textBoxTextColourId, juce::Colour(kLabelColour));
    magnitudeKnob_.textFromValueFunction = [](double v) { return juce::String (v, 2); };
    magnitudeKnob_.setLookAndFeel (&wiperLnf_);
    addAndMakeVisible (magnitudeKnob_);

    magnitudeLabel_.setText ("Magnitude", juce::dontSendNotification);
    magnitudeLabel_.setFont (FontHelper::getFont (13.0f));
    magnitudeLabel_.setColour (juce::Label::textColourId, juce::Colour (kLabelColour));
    magnitudeLabel_.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (magnitudeLabel_);

    // -----------------------------------------------------------------------
    // Phase knob
    // -----------------------------------------------------------------------
    phaseKnob_.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    phaseKnob_.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    phaseKnob_.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour(0x00000000));
    phaseKnob_.setColour (juce::Slider::textBoxTextColourId, juce::Colour(kLabelColour));
    phaseKnob_.textFromValueFunction = [](double v) { return juce::String (v, 2); };
    phaseKnob_.setLookAndFeel (&wiperLnf_);
    addAndMakeVisible (phaseKnob_);

    phaseLabel_.setText ("Phase", juce::dontSendNotification);
    phaseLabel_.setFont (FontHelper::getFont (13.0f));
    phaseLabel_.setColour (juce::Label::textColourId, juce::Colour (kLabelColour));
    phaseLabel_.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (phaseLabel_);

    // -----------------------------------------------------------------------
    // Threshold knob
    // -----------------------------------------------------------------------
    thresholdKnob_.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    thresholdKnob_.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    thresholdKnob_.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour(0x00000000));
    thresholdKnob_.setColour (juce::Slider::textBoxTextColourId, juce::Colour(kLabelColour));
    thresholdKnob_.textFromValueFunction = [](double v) { return juce::String (v, 1) + " dB"; };
    thresholdKnob_.setLookAndFeel (&wiperLnf_);
    addAndMakeVisible (thresholdKnob_);

    thresholdLabel_.setText ("Threshold", juce::dontSendNotification);
    thresholdLabel_.setFont (FontHelper::getFont (13.0f));
    thresholdLabel_.setColour (juce::Label::textColourId, juce::Colour (kLabelColour));
    thresholdLabel_.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (thresholdLabel_);

    // -----------------------------------------------------------------------
    // Bypass toggle buttons
    // -----------------------------------------------------------------------
    addAndMakeVisible (bypassLowButton_);
    bypassLowButton_.setLookAndFeel (&toggleLnf_);
    bypassLowButton_.setButtonText ("Bypass Lowest");
    addAndMakeVisible (bypassHighButton_);
    bypassHighButton_.setLookAndFeel (&toggleLnf_);
    bypassHighButton_.setButtonText ("Bypass Highest");

    // -----------------------------------------------------------------------
    // Slider range initialisation
    //
    // SliderAttachment normally configures the slider range from the
    // NormalisableRange stored in the APVTS parameter.  We must do this
    // manually because we are not using SliderAttachment.
    // -----------------------------------------------------------------------
    auto setSliderRange = [&] (juce::Slider& s, const juce::String& paramID)
    {
        auto* param = dynamic_cast<juce::RangedAudioParameter*> (apvts_.getParameter (paramID));
        if (param == nullptr)
            return;

        const auto& r = param->getNormalisableRange();
        s.setRange (static_cast<double> (r.start),
                    static_cast<double> (r.end),
                    static_cast<double> (r.interval));
        s.setValue (static_cast<double> (param->convertFrom0to1 (param->getValue())),
                    juce::dontSendNotification);
    };

    setSliderRange (magnitudeKnob_,  ParamID::Magnitude);
    setSliderRange (phaseKnob_,      ParamID::Phase);
    setSliderRange (thresholdKnob_,  ParamID::Threshold);

    // -----------------------------------------------------------------------
    // Button initial state
    // -----------------------------------------------------------------------
    auto setButtonState = [&] (juce::ToggleButton& b, const juce::String& paramID)
    {
        if (auto* param = apvts_.getParameter (paramID))
            b.setToggleState (param->getValue() >= 0.5f, juce::dontSendNotification);
    };

    setButtonState (bypassLowButton_,  ParamID::BypassLow);
    setButtonState (bypassHighButton_, ParamID::BypassHigh);

    // -----------------------------------------------------------------------
    // Polling setup: host → GUI direction
    //
    // Register each raw parameter pointer with the poller.  The poller reads
    // these atomics on the message thread at ~30 Hz and updates the widgets.
    // No AudioProcessorParameter::Listener is registered, so no audio-thread
    // heap allocation occurs during host automation.
    // -----------------------------------------------------------------------
    poller_.addSlider (apvts_.getRawParameterValue (ParamID::Magnitude),  &magnitudeKnob_);
    poller_.addSlider (apvts_.getRawParameterValue (ParamID::Phase),       &phaseKnob_);
    poller_.addSlider (apvts_.getRawParameterValue (ParamID::Threshold),   &thresholdKnob_);
    poller_.addButton (apvts_.getRawParameterValue (ParamID::BypassLow),   &bypassLowButton_);
    poller_.addButton (apvts_.getRawParameterValue (ParamID::BypassHigh),  &bypassHighButton_);

    poller_.startTimerHz (30);

    // -----------------------------------------------------------------------
    // Write-back lambdas: GUI → host direction
    //
    // When the user moves a knob or clicks a button, convert the displayed
    // value back to a normalised [0,1] value and call setValueNotifyingHost()
    // so the host can record automation.  These callbacks fire on the message
    // thread, which is safe.
    // -----------------------------------------------------------------------
    magnitudeKnob_.onValueChange = [this]
    {
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (
                          apvts_.getParameter (ParamID::Magnitude)))
            p->setValueNotifyingHost (
                p->convertTo0to1 (static_cast<float> (magnitudeKnob_.getValue())));
    };

    phaseKnob_.onValueChange = [this]
    {
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (
                          apvts_.getParameter (ParamID::Phase)))
            p->setValueNotifyingHost (
                p->convertTo0to1 (static_cast<float> (phaseKnob_.getValue())));
    };

    thresholdKnob_.onValueChange = [this]
    {
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (
                          apvts_.getParameter (ParamID::Threshold)))
            p->setValueNotifyingHost (
                p->convertTo0to1 (static_cast<float> (thresholdKnob_.getValue())));
    };

    bypassLowButton_.onStateChange = [this]
    {
        if (auto* p = apvts_.getParameter (ParamID::BypassLow))
            p->setValueNotifyingHost (bypassLowButton_.getToggleState() ? 1.0f : 0.0f);
    };

    bypassHighButton_.onStateChange = [this]
    {
        if (auto* p = apvts_.getParameter (ParamID::BypassHigh))
            p->setValueNotifyingHost (bypassHighButton_.getToggleState() ? 1.0f : 0.0f);
    };
}

//==============================================================================
// Destructor
//==============================================================================

MainView::~MainView()
{
    // Remove all child components before member components are destroyed.
    // Component::~Component() would otherwise call removeAllChildren() after
    // all members are already gone, causing use-after-free / heap corruption.
    removeAllChildren();

    // Stop the parameter poller before any member is destroyed.
    poller_.stopTimer();

    // Clear LookAndFeel references to avoid dangling pointer on destruction.
    magnitudeKnob_.setLookAndFeel (nullptr);
    phaseKnob_.setLookAndFeel     (nullptr);
    thresholdKnob_.setLookAndFeel (nullptr);

    settingsButton_.setLookAndFeel (nullptr);
    aboutButton_.setLookAndFeel    (nullptr);
    bypassLowButton_.setLookAndFeel (nullptr);
    bypassHighButton_.setLookAndFeel (nullptr);
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
    constexpr int kAboutButtonWidth = 48; // "Info"
    constexpr int kSettingsButtonWidth = 52; // "Freq"
    constexpr int kNavButtonHeight = 26;
    constexpr int kNavButtonMargin = (kHeaderHeight - kNavButtonHeight) / 2;
    constexpr int kSpacing = 8;
    
    // About button — far right
    aboutButton_.setBounds (w - kAboutButtonWidth - 16,
                            kNavButtonMargin,
                            kAboutButtonWidth,
                            kNavButtonHeight);

    // Settings/Bands button — left of About
    settingsButton_.setBounds (aboutButton_.getX() - kSettingsButtonWidth - kSpacing,
                               kNavButtonMargin,
                               kSettingsButtonWidth,
                               kNavButtonHeight);

    // Title — occupies the rest of the header
    titleLabel_.setBounds (16, 0, w - 150, kHeaderHeight);

    // -----------------------------------------------------------------------
    // Bottom section heights
    // -----------------------------------------------------------------------
    const int bottomSectionHeight = 160;

    // -----------------------------------------------------------------------
    // Spectrum widget — central area between header and bottom section
    // -----------------------------------------------------------------------
    spectrumWidget_.setBounds (0, kHeaderHeight, w, h - kHeaderHeight - bottomSectionHeight);

    // -----------------------------------------------------------------------
    // Labels -> Knobs -> Checkboxes
    // -----------------------------------------------------------------------
    const int bottomY = h - bottomSectionHeight;
    const int colWidth = w / 3;

    constexpr int kTitleHeight = 20;
    constexpr int kKnobTotalHeight = 84; 
    constexpr int kCheckboxHeight = 24;

    const int titleY = bottomY + 16;
    magnitudeLabel_.setBounds (0,            titleY, colWidth, kTitleHeight);
    phaseLabel_.setBounds     (colWidth,     titleY, colWidth, kTitleHeight);
    thresholdLabel_.setBounds (colWidth * 2, titleY, colWidth, kTitleHeight);

    const int knobY = titleY + kTitleHeight + 2;
    magnitudeKnob_.setBounds (0,            knobY, colWidth, kKnobTotalHeight);
    phaseKnob_.setBounds     (colWidth,     knobY, colWidth, kKnobTotalHeight);
    thresholdKnob_.setBounds (colWidth * 2, knobY, colWidth, kKnobTotalHeight);

    const int checkY = bottomY + bottomSectionHeight - kCheckboxHeight - 14;
    const int checkWidth = w / 2;
    bypassLowButton_.setBounds  (24, checkY, checkWidth - 24, kCheckboxHeight);
    bypassHighButton_.setBounds (checkWidth + 12, checkY, checkWidth - 12, kCheckboxHeight);
}
