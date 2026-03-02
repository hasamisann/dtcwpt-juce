/**
 * @file AboutView.cpp
 * @brief Static "About" page implementation for the DT-CWPT Morph plugin.
 */

#include "AboutView.h"

//==============================================================================
// Colours (anonymous namespace)
//==============================================================================

namespace
{
    /** Background fill colour: very dark blue-grey. */
    constexpr juce::uint32 kBgColour         = 0xFF1A1A2E;

    /** Divider line colour. */
    constexpr juce::uint32 kDividerColour    = 0xFF3A3A5A;

    /** Primary text colour: off-white. */
    constexpr juce::uint32 kTextColour       = 0xFFDDDDDD;

    /** Secondary / muted text colour. */
    constexpr juce::uint32 kMutedTextColour  = 0xFF888888;

    /** Hyperlink colour: accent blue. */
    constexpr juce::uint32 kLinkColour       = 0xFF4A9FFF;

} // namespace

//==============================================================================
// Constructor
//==============================================================================

AboutView::AboutView (std::function<void()> onBack)
    : githubLink_     ("GitHub: hasamisann",    juce::URL (kGitHubURL))
    , twitterLink_    ("X: hasamisann",         juce::URL (kTwitterURL))
    , soundcloudLink_ ("SoundCloud: hasamisann", juce::URL (kSoundCloudURL))
    , onBack_         (std::move (onBack))
{
    // -----------------------------------------------------------------------
    // Plugin name label
    // -----------------------------------------------------------------------
    pluginNameLabel_.setText ("DT-CWPT Morph", juce::dontSendNotification);
    pluginNameLabel_.setFont (juce::Font (28.0f, juce::Font::bold));
    pluginNameLabel_.setColour (juce::Label::textColourId, juce::Colour (kTextColour));
    pluginNameLabel_.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (pluginNameLabel_);

    // -----------------------------------------------------------------------
    // Version label
    // -----------------------------------------------------------------------
    versionLabel_.setText ("Version 0.1.0", juce::dontSendNotification);
    versionLabel_.setFont (juce::Font (14.0f));
    versionLabel_.setColour (juce::Label::textColourId, juce::Colour (kMutedTextColour));
    versionLabel_.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (versionLabel_);

    // -----------------------------------------------------------------------
    // Hyperlink buttons
    // -----------------------------------------------------------------------
    const juce::Colour linkColour (kLinkColour);

    githubLink_.setColour (juce::HyperlinkButton::textColourId, linkColour);
    githubLink_.setFont (juce::Font (13.0f), false, juce::Justification::centred);
    addAndMakeVisible (githubLink_);

    twitterLink_.setColour (juce::HyperlinkButton::textColourId, linkColour);
    twitterLink_.setFont (juce::Font (13.0f), false, juce::Justification::centred);
    addAndMakeVisible (twitterLink_);

    soundcloudLink_.setColour (juce::HyperlinkButton::textColourId, linkColour);
    soundcloudLink_.setFont (juce::Font (13.0f), false, juce::Justification::centred);
    addAndMakeVisible (soundcloudLink_);

    // -----------------------------------------------------------------------
    // Trademark label
    // -----------------------------------------------------------------------
    trademarkLabel_.setText (kTrademarkText, juce::dontSendNotification);
    trademarkLabel_.setFont (juce::Font (10.0f));
    trademarkLabel_.setColour (juce::Label::textColourId, juce::Colour (kMutedTextColour));
    trademarkLabel_.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (trademarkLabel_);

    // -----------------------------------------------------------------------
    // Back button
    // -----------------------------------------------------------------------
    backButton_.setButtonText ("< Back");
    backButton_.onClick = [this]
    {
        if (onBack_) onBack_();
    };
    addAndMakeVisible (backButton_);
}

//==============================================================================
// juce::Component overrides
//==============================================================================

void AboutView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (kBgColour));

    // Subtle horizontal divider below the plugin name label
    const int dividerY = pluginNameLabel_.getBottom() + 6;
    g.setColour (juce::Colour (kDividerColour));
    g.drawHorizontalLine (dividerY,
                          static_cast<float> (getWidth()) * 0.1f,
                          static_cast<float> (getWidth()) * 0.9f);
}

void AboutView::resized()
{
    constexpr int kRowH   = 28;
    constexpr int kGap    = 10;
    constexpr int kLinkH  = 24;
    constexpr int kBtnH   = 28;
    constexpr int kBtnW   = 80;

    const int w = getWidth();

    // Back button — top-left
    backButton_.setBounds (6, 6, kBtnW, kBtnH);

    // Vertical stack, centred in the component
    const int totalH = kRowH          // plugin name
                     + kGap
                     + kRowH          // version
                     + kGap * 2
                     + kLinkH         // github
                     + kGap
                     + kLinkH         // twitter
                     + kGap
                     + kLinkH         // soundcloud
                     + kGap * 2
                     + kRowH;         // trademark

    int y = (getHeight() - totalH) / 2;
    if (y < 44) y = 44;  // don't overlap back button

    pluginNameLabel_.setBounds (0, y, w, kRowH);
    y += kRowH + kGap;

    versionLabel_.setBounds (0, y, w, kRowH);
    y += kRowH + kGap * 2;

    githubLink_.setBounds (w / 4, y, w / 2, kLinkH);
    y += kLinkH + kGap;

    twitterLink_.setBounds (w / 4, y, w / 2, kLinkH);
    y += kLinkH + kGap;

    soundcloudLink_.setBounds (w / 4, y, w / 2, kLinkH);
    y += kLinkH + kGap * 2;

    trademarkLabel_.setBounds (0, y, w, kRowH);
}
