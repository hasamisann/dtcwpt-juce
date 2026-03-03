#include "GateMainView.h"
#include "FontHelper.h"

namespace
{
    const juce::Colour kBgColor     = juce::Colour (18, 18, 27);
    const juce::Colour kHeaderColor = juce::Colour (27, 27, 36);
    const int kHeaderHeight = 32;
    const int kSpectrumHeight = 100;
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
    titleLabel_.setFont (FontHelper::getFont (14.0f, true));
    titleLabel_.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel_);

    // Setup buttons
    settingsButton_.setButtonText (juce::CharPointer_UTF8("\xe2\x9a\x99")); // ⚙
    settingsButton_.onClick = [this] { if (onSettingsClicked) onSettingsClicked(); };
    addAndMakeVisible (settingsButton_);

    aboutButton_.setButtonText (juce::CharPointer_UTF8("\xe2\x84\xb9")); // ℹ
    aboutButton_.onClick = [this] { if (onAboutClicked) onAboutClicked(); };
    addAndMakeVisible (aboutButton_);

    // Customise button looks
    struct HeaderButtonLookAndFeel : public juce::LookAndFeel_V4
    {
        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override {}
        
        void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool /*isMouseOverButton*/, bool /*isButtonDown*/) override
        {
            g.setFont (FontHelper::getFont (14.0f));
            g.setColour (juce::Colour (220, 220, 220));
            g.drawText (button.getButtonText(), button.getLocalBounds(), juce::Justification::centred, false);
        }
    };
    
    // Allocate a shared LookAndFeel for these buttons
    // Since this is a simple port, we can use a static instance for simplicity,
    // though normally a member LookAndFeel object is safer.
    static HeaderButtonLookAndFeel lnf;
    settingsButton_.setLookAndFeel (&lnf);
    aboutButton_.setLookAndFeel (&lnf);

    addAndMakeVisible (spectrumWidget_);
    addAndMakeVisible (graphicEqWidget_);
}

GateMainView::~GateMainView()
{
    settingsButton_.setLookAndFeel (nullptr);
    aboutButton_.setLookAndFeel (nullptr);
}

void GateMainView::paint (juce::Graphics& g)
{
    g.fillAll (kBgColor);
    
    // Draw header background
    g.setColour (kHeaderColor);
    g.fillRect (0, 0, getWidth(), kHeaderHeight);
}

void GateMainView::resized()
{
    auto bounds = getLocalBounds();
    
    // Header
    auto headerBounds = bounds.removeFromTop (kHeaderHeight);
    
    // Margins matching Rust (inner_margin (12, 8)) -> left/right 12, top/bottom 8
    headerBounds.reduce (12, 0); 
    
    titleLabel_.setBounds (headerBounds.removeFromLeft (200).withSizeKeepingCentre (200, 20));
    
    // Buttons right-aligned
    aboutButton_.setBounds (headerBounds.removeFromRight (24).withSizeKeepingCentre (24, 24));
    headerBounds.removeFromRight (12); // spacing
    settingsButton_.setBounds (headerBounds.removeFromRight (24).withSizeKeepingCentre (24, 24));

    // Spectrum
    bounds.removeFromTop (10); // add_space(10.0)
    spectrumWidget_.setBounds (bounds.removeFromTop (kSpectrumHeight));
    
    // Graphic EQ
    bounds.removeFromTop (10); // add_space(10.0)
    graphicEqWidget_.setBounds (bounds);
}
