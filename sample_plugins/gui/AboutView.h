#pragma once

/**
 * @file AboutView.h
 * @brief Static "About" page for the DT-CWPT Morph plugin.
 *
 * Displays plugin name, version string, social-media hyperlinks, VST3 trademark
 * notice, and a back button that invokes the supplied callback to return to the
 * main view.
 *
 * Design per SPEC.md US-007 acceptance criteria:
 *  - Plugin name "DT-CWPT Morph" in large bold text.
 *  - Version string "Version 0.1.0".
 *  - Hyperlinks to GitHub, X (Twitter), and SoundCloud profiles.
 *  - VST3 trademark notice.
 *  - Back button that fires a caller-supplied callback.
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

/**
 * @brief Static informational component shown in the About view.
 *
 * All child components are created and laid out in the constructor and resized().
 * The back-button callback is passed at construction time and stored as a
 * std::function<void()>.
 *
 * Usage:
 * @code
 *   auto about = std::make_unique<AboutView>([this]{ showMainView(); });
 * @endcode
 */
class AboutView : public juce::Component
{
public:
    //==============================================================================
    // Constructor / Destructor
    //==============================================================================

    /**
     * @brief Constructs the AboutView.
     *
     * @param onBack  Callback invoked when the user clicks the Back button.
     *                Called on the message thread. May be nullptr (button is still
     *                shown but does nothing).
     */
    explicit AboutView (std::function<void()> onBack);

    /** Destructor. Removes child components before member sub-components are destroyed. */
    ~AboutView() override;

    //==============================================================================
    // juce::Component overrides
    //==============================================================================

    /** Fills background and draws a subtle divider below the plugin name. */
    void paint (juce::Graphics& g) override;

    /** Positions all child components in a centred vertical column. */
    void resized() override;

private:
    //==============================================================================
    // URL constants
    //==============================================================================

    /** GitHub profile URL. */
    static constexpr const char* kGitHubURL    = "https://github.com/hasamisann/dtcwpt-juce";

    /** X (Twitter) profile URL. */
    static constexpr const char* kTwitterURL   = "https://x.com/LTSU_n_nv";

    /** SoundCloud profile URL. */
    static constexpr const char* kSoundCloudURL = "https://soundcloud.com/LTSU_n_nv";

    /** VST3 trademark notice text. */
    static constexpr const char* kTrademarkText =
        "VST is a registered trademark of Steinberg Media Technologies GmbH";

    //==============================================================================
    // Child components
    //==============================================================================

    /** "DT-CWPT Morph" — large bold label. */
    juce::Label pluginNameLabel_;

    /** "Version 0.1.0" */
    juce::Label versionLabel_;

    /** "GitHub: hasamisann" hyperlink. */
    juce::HyperlinkButton githubLink_;

    /** "X: hasamisann" hyperlink. */
    juce::HyperlinkButton twitterLink_;

    /** "SoundCloud: hasamisann" hyperlink. */
    juce::HyperlinkButton soundcloudLink_;

    /** VST3 trademark notice label. */
    juce::Label trademarkLabel_;

    /** Back button — returns to the main view. */
    juce::TextButton backButton_;

    /** Callback invoked on back-button click. */
    std::function<void()> onBack_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutView)
};
