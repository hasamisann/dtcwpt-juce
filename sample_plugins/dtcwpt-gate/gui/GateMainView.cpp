#include "GateMainView.h"
#include "FontHelper.h"

namespace
{
    const juce::Colour kBgColor     = juce::Colour (0xFF202026);
    const int kHeaderHeight = 60;
    const int kSpectrumHeight = 100;
    const int kDynamicsRowHeight = 72;

    struct DynamicsKnobSpec { const char* parameterId; const char* caption; };
    const DynamicsKnobSpec kDynamicsKnobs[] = {
        { "attack", "Attack" }, { "hold", "Hold" }, { "release", "Release" },
        { "hysteresis", "Hysteresis" }, { "range", "Range" }
    };
}

//==============================================================================
GateMainView::GateMainView (GateAudioProcessor& processor, double sampleRate)
    : processor_ (processor),
      spectrumWidget_ (processor.getSpectrumBridge()),
      graphicEqWidget_ (processor, static_cast<float>(sampleRate))
{
    spectrumWidget_.setSampleRate (sampleRate);
    // Setup title
    titleLabel_.setText ("DT-CWPT Gate", juce::dontSendNotification);
    titleLabel_.setFont (FontHelper::getFont (24.0f, false, 0.05f));
    titleLabel_.setColour (juce::Label::textColourId, juce::Colour (0xFFDDDDDD));
    titleLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel_);

    // Setup buttons
    settingsButton_.setButtonText ("Freq");
    settingsButton_.onClick = [this] { if (onSettingsClicked) onSettingsClicked(); };
    addAndMakeVisible (settingsButton_);

    aboutButton_.setButtonText ("Info");
    aboutButton_.onClick = [this] { if (onAboutClicked) onAboutClicked(); };
    addAndMakeVisible (aboutButton_);

    bypassLowestButton_.setButtonText ("Bypass Lowest");
    bypassLowestButton_.setColour (juce::ToggleButton::textColourId, juce::Colour (0xFFF0F0F0));
    addAndMakeVisible (bypassLowestButton_);
    
    bypassAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor_.getAPVTS(), "bypass_lowest_band", bypassLowestButton_
    );

    // Customise button looks
    struct HeaderButtonLookAndFeel : public juce::LookAndFeel_V4
    {
        HeaderButtonLookAndFeel()
        {
            setColour (juce::TextButton::buttonColourId,   juce::Colour (0xFF2E2E2F));
            setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xFF2E2E2F));
            setColour (juce::TextButton::textColourOffId,  juce::Colour (0xFFF0F0F0));
            setColour (juce::TextButton::textColourOnId,   juce::Colour (0xFFF0F0F0));
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                   const juce::Colour& backgroundColour,
                                   bool, bool) override
        {
            auto bounds = button.getLocalBounds().toFloat();
            g.setColour (backgroundColour);
            g.fillRect (bounds);
        }

        juce::Font getTextButtonFont (juce::TextButton&, int) override
        {
            return FontHelper::getFont (13.0f);
        }
    };
    
    static HeaderButtonLookAndFeel lnf;
    settingsButton_.setLookAndFeel (&lnf);
    aboutButton_.setLookAndFeel (&lnf);

    struct GateToggleButtonLookAndFeel : public juce::LookAndFeel_V4
    {
        void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                               bool, bool) override
        {
            auto font = FontHelper::getFont (12.0f);
            auto tickWidth = 14.0f;
            auto tickY = (static_cast<float> (button.getHeight()) - tickWidth) * 0.5f;
            auto tickX = 4.0f;

            juce::Rectangle<float> box (tickX, tickY, tickWidth, tickWidth);
            g.setColour (juce::Colour (0xFFB7BCC1));
            g.drawRect (box, 1.5f);

            if (button.getToggleState())
            {
                g.setColour (juce::Colour (0xFFB7BCC1));
                g.fillRect (box.reduced (3.0f));
            }

            g.setColour (juce::Colour (0xFFF0F0F0));
            g.setFont (font);
            g.drawText (button.getButtonText(),
                        static_cast<int> (tickX + tickWidth + 6.0f), 0,
                        button.getWidth() - static_cast<int> (tickWidth) - 10, button.getHeight(),
                        juce::Justification::centredLeft, true);
        }
    };
    static GateToggleButtonLookAndFeel toggleLnf;
    bypassLowestButton_.setLookAndFeel (&toggleLnf);

    // Gate dynamics controls
    stereoLinkButton_.setButtonText ("Stereo Link");
    stereoLinkButton_.setLookAndFeel (&toggleLnf);
    addAndMakeVisible (stereoLinkButton_);
    stereoLinkAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor_.getAPVTS(), "stereo_link", stereoLinkButton_
    );

    for (int i = 0; i < kNumDynamicsKnobs; ++i)
    {
        auto& knob = dynamicsKnobs_[static_cast<size_t> (i)];

        knob.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 14);
        knob.slider.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xFFB7BCC1));
        knob.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xFF2E2E2F));
        knob.slider.setColour (juce::Slider::thumbColourId, juce::Colour (0xFFF0F0F0));
        knob.slider.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xFFF0F0F0));
        knob.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (knob.slider);

        knob.caption.setText (kDynamicsKnobs[i].caption, juce::dontSendNotification);
        knob.caption.setFont (FontHelper::getFont (12.0f));
        knob.caption.setColour (juce::Label::textColourId, juce::Colour (0xFFB7BCC1));
        knob.caption.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (knob.caption);

        knob.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor_.getAPVTS(), kDynamicsKnobs[i].parameterId, knob.slider
        );
    }

    addAndMakeVisible (spectrumWidget_);
    addAndMakeVisible (graphicEqWidget_);
}

GateMainView::~GateMainView()
{
    settingsButton_.setLookAndFeel (nullptr);
    aboutButton_.setLookAndFeel (nullptr);
    bypassLowestButton_.setLookAndFeel (nullptr);
    stereoLinkButton_.setLookAndFeel (nullptr);
}

void GateMainView::paint (juce::Graphics& g)
{
    g.fillAll (kBgColor);
}

void GateMainView::resized()
{
    auto bounds = getLocalBounds();
    
    // Header
    auto headerBounds = bounds.removeFromTop (kHeaderHeight);
    
    // Margins matching Rust (inner_margin (12, 8)) -> left/right 12, top/bottom 8
    headerBounds.reduce (12, 0); 
    
    titleLabel_.setBounds (headerBounds.removeFromLeft (200).withSizeKeepingCentre (200, 30));
    
    // Buttons right-aligned
    aboutButton_.setBounds (headerBounds.removeFromRight (60).withSizeKeepingCentre (60, 24));
    headerBounds.removeFromRight (8); // spacing
    settingsButton_.setBounds (headerBounds.removeFromRight (60).withSizeKeepingCentre (60, 24));
    headerBounds.removeFromRight (8);
    bypassLowestButton_.setBounds (headerBounds.removeFromRight (110).withSizeKeepingCentre (110, 24));

    // Spectrum
    bounds.removeFromTop (10); // add_space(10.0)
    spectrumWidget_.setBounds (bounds.removeFromTop (kSpectrumHeight));
    
    // Gate dynamics row: five knobs and the stereo link toggle
    bounds.removeFromTop (6);
    auto dynamicsBounds = bounds.removeFromTop (kDynamicsRowHeight).reduced (12, 0);
    stereoLinkButton_.setBounds (dynamicsBounds.removeFromRight (110).withSizeKeepingCentre (110, 24));
    const int knobWidth = dynamicsBounds.getWidth() / kNumDynamicsKnobs;
    for (auto& knob : dynamicsKnobs_)
    {
        auto cell = dynamicsBounds.removeFromLeft (knobWidth);
        knob.caption.setBounds (cell.removeFromTop (14));
        knob.slider.setBounds (cell);
    }

    // Graphic EQ
    bounds.removeFromTop (10); // add_space(10.0)
    graphicEqWidget_.setBounds (bounds);
}
