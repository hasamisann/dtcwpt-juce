/**
 * @file SpectrumAnalyzerWidget.cpp
 * @brief FFT-based logarithmic spectrum analyzer display implementation.
 */

#include "SpectrumAnalyzerWidget.h"

#include <algorithm>
#include <cmath>

//==============================================================================
// Constants (anonymous namespace)
//==============================================================================

namespace
{
    /** Frequency grid lines to draw (Hz). */
    constexpr float kFreqGridHz[]  = { 100.0f, 1000.0f, 10000.0f };

    /** dB grid lines to draw. */
    constexpr float kDbGridLevels[] = { -60.0f, -40.0f, -20.0f, 0.0f };

    /** Colour for the input spectrum (grey). */
    constexpr juce::uint32 kInputColour  = 0x99999999;

    /** Colour for the output spectrum (cyan/blue). */
    constexpr juce::uint32 kOutputColour = 0xB24A9FFF;

    /** Background fill colour. */
    constexpr juce::uint32 kBackgroundColour = 0xFF1A1A1A;

    /** Grid line colour. */
    constexpr juce::uint32 kGridColour = 0x33FFFFFF;
} // namespace

//==============================================================================
// Constructor / Destructor
//==============================================================================

SpectrumAnalyzerWidget::SpectrumAnalyzerWidget (SpectrumDataBridge& bridge)
    : bridge_ (bridge)
{
    // Zero out smoothed arrays
    inputSmoothed_.fill  (kMinDB);
    outputSmoothed_.fill (kMinDB);

    // Start polling timer at ~30 Hz
    startTimer (kTimerIntervalMs);
}

SpectrumAnalyzerWidget::~SpectrumAnalyzerWidget()
{
    stopTimer();
}

//==============================================================================
// Configuration
//==============================================================================

void SpectrumAnalyzerWidget::setSmoothingFactor (float alpha) noexcept
{
    smoothingAlpha_ = std::clamp (alpha, 0.0f, 1.0f);
}

void SpectrumAnalyzerWidget::setSampleRate (double sampleRate) noexcept
{
    maxFreqHz_ = static_cast<float> (sampleRate * 0.5);
}

//==============================================================================
// Timer callback
//==============================================================================

void SpectrumAnalyzerWidget::timerCallback()
{
    if (bridge_.tryConsume (inputStageBuf_, outputStageBuf_))
    {
        processFFT (inputStageBuf_,  inputSmoothed_);
        processFFT (outputStageBuf_, outputSmoothed_);
        repaint();
    }
}

//==============================================================================
// Paint
//==============================================================================

void SpectrumAnalyzerWidget::paint (juce::Graphics& g)
{
    const float w = static_cast<float> (getWidth());
    const float h = static_cast<float> (getHeight());

    // Background
    g.fillAll (juce::Colour (kBackgroundColour));

    // Grid
    drawGrid (g, w, h);

    // Input spectrum (gray, 50% alpha)
    drawSpectrumPath (g, inputSmoothed_,
                      juce::Colour (kInputColour).withAlpha (0.5f),
                      w, h);

    // Output spectrum (blue, 70% alpha)
    drawSpectrumPath (g, outputSmoothed_,
                      juce::Colour (kOutputColour).withAlpha (0.7f),
                      w, h);
}

//==============================================================================
// Private helpers
//==============================================================================

void SpectrumAnalyzerWidget::processFFT (const std::array<float, kFFTSize>& rawBuf,
                                          std::array<float, kFFTSize / 2>&   smoothedOut)
{
    // Copy input into working buffer (zero-padded to 2× size)
    fftWorkBuf_.fill (0.0f);
    std::copy (rawBuf.begin(), rawBuf.end(), fftWorkBuf_.begin());

    // Apply Hann window
    window_.multiplyWithWindowingTable (fftWorkBuf_.data(),
                                        static_cast<std::size_t> (kFFTSize));

    // Forward FFT (real → complex, output in first kFFTSize complex pairs)
    fft_.performRealOnlyForwardTransform (fftWorkBuf_.data());

    // Convert complex FFT output to dB and apply exponential smoothing
    const float alpha = smoothingAlpha_;
    const float oneMinusAlpha = 1.0f - alpha;

    for (int i = 0; i < kFFTSize / 2; ++i)
    {
        const float re  = fftWorkBuf_[static_cast<std::size_t> (i * 2)];
        const float im  = fftWorkBuf_[static_cast<std::size_t> (i * 2 + 1)];
        const float mag = std::sqrt (re * re + im * im) / static_cast<float> (kFFTSize);
        const float dB  = (mag > 1e-10f)
                              ? 20.0f * std::log10 (mag)
                              : kMinDB;
        const float clampedDB = std::clamp (dB, kMinDB, kMaxDB);

        // Exponential smoothing: new = alpha * old + (1 - alpha) * new
        smoothedOut[static_cast<std::size_t> (i)] =
            alpha * smoothedOut[static_cast<std::size_t> (i)]
            + oneMinusAlpha * clampedDB;
    }
}

float SpectrumAnalyzerWidget::freqToX (float freqHz, float width) const noexcept
{
    const float minFreq = kMinFreqHz;
    const float maxFreq = maxFreqHz_;

    if (freqHz <= minFreq) return 0.0f;
    if (freqHz >= maxFreq) return width;

    const float logMin   = std::log (minFreq);
    const float logMax   = std::log (maxFreq);
    const float logFreq  = std::log (freqHz);

    return (logFreq - logMin) / (logMax - logMin) * width;
}

float SpectrumAnalyzerWidget::dbToY (float dB, float height) const noexcept
{
    const float t = (dB - kMinDB) / (kMaxDB - kMinDB);   // 0 at minDB, 1 at maxDB
    return (1.0f - t) * height;                           // flip: 0 dB at top
}

void SpectrumAnalyzerWidget::drawSpectrumPath (juce::Graphics& g,
                                                const std::array<float, kFFTSize / 2>& smoothed,
                                                juce::Colour colour,
                                                float width,
                                                float height)
{
    juce::Path path;
    bool pathStarted = false;

    const int numBins = kFFTSize / 2;
    const float nyquist = maxFreqHz_;

    for (int i = 1; i < numBins; ++i)
    {
        // Map bin index to frequency
        const float freqHz = (static_cast<float> (i) / static_cast<float> (numBins)) * nyquist;

        if (freqHz < kMinFreqHz) continue;

        const float x = freqToX (freqHz, width);
        const float y = dbToY (smoothed[static_cast<std::size_t> (i)], height);

        if (!pathStarted)
        {
            path.startNewSubPath (x, y);
            pathStarted = true;
        }
        else
        {
            path.lineTo (x, y);
        }
    }

    if (pathStarted)
    {
        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (1.5f));
    }
}

void SpectrumAnalyzerWidget::drawGrid (juce::Graphics& g, float width, float height)
{
    g.setColour (juce::Colour (kGridColour));

    const juce::Font labelFont (10.0f);
    g.setFont (labelFont);

    // Frequency grid lines (vertical)
    for (float freqHz : kFreqGridHz)
    {
        if (freqHz >= maxFreqHz_) continue;

        const float x = freqToX (freqHz, width);
        g.drawLine (x, 0.0f, x, height, 0.5f);

        // Label
        const juce::String label = (freqHz >= 1000.0f)
                                       ? juce::String (static_cast<int> (freqHz / 1000.0f)) + "k"
                                       : juce::String (static_cast<int> (freqHz));
        g.drawText (label,
                    static_cast<int> (x) + 2, 2, 30, 12,
                    juce::Justification::left, false);
    }

    // dB grid lines (horizontal)
    for (float dB : kDbGridLevels)
    {
        const float y = dbToY (dB, height);
        g.drawLine (0.0f, y, width, y, 0.5f);

        const juce::String label = juce::String (static_cast<int> (dB)) + "dB";
        g.drawText (label,
                    2, static_cast<int> (y) - 10, 36, 10,
                    juce::Justification::left, false);
    }
}
