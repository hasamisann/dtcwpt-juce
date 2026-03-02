#pragma once

/**
 * @file FontHelper.h
 * @brief Header-only utility for loading Hanken Grotesk typefaces from BinaryData.
 *
 * Usage:
 * @code
 *   g.setFont(FontHelper::getFont(14.0f));
 *   g.setFont(FontHelper::getFont(16.0f, true)); // bold
 * @endcode
 *
 * The typefaces are lazily initialised on first call and cached as static
 * juce::Typeface::Ptr objects (thread-safe via C++11 magic statics).
 *
 * Font files are embedded via the DtcwptMorphResources binary data target
 * (see CMakeLists.txt). The SIL OFL v1.1 license for Hanken Grotesk is
 * stored in BinaryData as OFL_txt.
 */

#include <BinaryData.h>
#include <juce_graphics/juce_graphics.h>

namespace FontHelper
{
    /**
     * @brief Returns a Hanken Grotesk font at the given height.
     *
     * @param height  Desired font height in points.
     * @param bold    If true, returns the Bold weight; otherwise Regular.
     * @return        A juce::Font configured with the appropriate typeface.
     */
    inline juce::Font getFont (float height, bool bold = false)
    {
        static const juce::Typeface::Ptr regular =
            juce::Typeface::createSystemTypefaceFor (
                BinaryData::HankenGroteskRegular_ttf,
                BinaryData::HankenGroteskRegular_ttfSize);

        static const juce::Typeface::Ptr boldFace =
            juce::Typeface::createSystemTypefaceFor (
                BinaryData::HankenGroteskBold_ttf,
                BinaryData::HankenGroteskBold_ttfSize);

        return juce::Font (juce::FontOptions()
                               .withTypeface (bold ? boldFace : regular)
                               .withHeight (height));
    }

} // namespace FontHelper
