#pragma once

/**
 * @file SpectrumAnalyzerWidget.h
 * @brief FFT-based logarithmic spectrum analyzer display.
 *
 * SpectrumAnalyzerWidget polls a SpectrumDataBridge at ~30 Hz, applies a 2048-point
 * Hann-windowed FFT to the latest audio samples, and paints overlaid input (gray)
 * and output (blue) spectra on a logarithmic frequency axis.
 *
 * Design per SPEC.md US-005:
 *  - FFT size 2048, Hann window.
 *  - Logarithmic x-axis (15 Hz to Nyquist), dB y-axis (-80 dB to 0 dB).
 *  - Exponential smoothing with configurable alpha.
 *  - Input shown in grey, output shown in cyan/blue.
 *
 * Thread safety: All methods are called on the GUI / message thread. The bridge
 * provides the thread-safe handoff from the audio thread.
 */

#include "SpectrumDataBridge.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

/**
 * @brief Real-time spectrum analyzer component using SpectrumDataBridge.
 *
 * Inherits juce::Component for painting and juce::Timer (privately) for periodic
 * polling of the bridge at approximately 30 Hz.
 */
class SpectrumAnalyzerWidget : public juce::Component,
                                private juce::Timer
{
public:
    //==============================================================================
    // Constants
    //==============================================================================

    /** FFT order: 2^11 = 2048 samples. */
    static constexpr int kFFTOrder = 11;

    /** FFT size. */
    static constexpr int kFFTSize = 1 << kFFTOrder;

    /** Minimum display frequency (Hz). */
    static constexpr float kMinFreqHz = 15.0f;

    /** Maximum display frequency — set at runtime from sample rate (Nyquist). */
    static constexpr float kDefaultMaxFreqHz = 22050.0f;

    /** Minimum dB level for display. */
    static constexpr float kMinDB = -80.0f;

    /** Maximum dB level for display. */
    static constexpr float kMaxDB = 0.0f;

    /** Timer interval in milliseconds (~30 Hz). */
    static constexpr int kTimerIntervalMs = 33;

    //==============================================================================
    // Constructor / Destructor
    //==============================================================================

    /**
     * @brief Constructs the widget.
     *
     * @param bridge  Reference to the SpectrumDataBridge owned by MorphAudioProcessor.
     *                The bridge must outlive this widget.
     */
    explicit SpectrumAnalyzerWidget (SpectrumDataBridge& bridge);

    /** Destructor — stops the timer. */
    ~SpectrumAnalyzerWidget() override;

    //==============================================================================
    // Configuration
    //==============================================================================

    /**
     * @brief Sets the exponential smoothing factor for spectrum display.
     *
     * @param alpha  Smoothing coefficient in [0, 1].
     *               0 = no smoothing (raw FFT), 1 = infinitely smooth (frozen).
     *               Default: 0.8.
     */
    void setSmoothingFactor (float alpha) noexcept;

    /**
     * @brief Sets the Nyquist frequency for the frequency axis mapping.
     *
     * @param sampleRate  Host sample rate in Hz. Nyquist = sampleRate / 2.
     */
    void setSampleRate (double sampleRate) noexcept;

protected:
    //==============================================================================
    // juce::Component overrides
    //==============================================================================

    /** Paints the spectrum display. */
    void paint (juce::Graphics& g) override;

    //==============================================================================
    // juce::Timer override (private inheritance)
    //==============================================================================

    /**
     * @brief Called at ~30 Hz.
     *
     * Polls the bridge, applies FFT + windowing if new data is available,
     * updates smoothed spectrum arrays, and triggers repaint().
     */
    void timerCallback() override;

private:
    //==============================================================================
    // Helpers
    //==============================================================================

    /** Applies FFT to rawBuf, converts to dB, applies exponential smoothing. */
    void processFFT (const std::array<float, kFFTSize>& rawBuf,
                     std::array<float, kFFTSize / 2>&   smoothedOut);

    /** Maps a frequency value to an x-pixel using log scale. */
    float freqToX (float freqHz, float width) const noexcept;

    /** Maps a dB value to a y-pixel (top = 0 dB, bottom = kMinDB). */
    float dbToY (float dB, float height) const noexcept;

    /** Draws one spectrum path (input or output). */
    void drawSpectrumPath (juce::Graphics& g,
                           const std::array<float, kFFTSize / 2>& smoothed,
                           juce::Colour colour,
                           float width,
                           float height);

    /** Draws logarithmic frequency grid lines and dB grid lines. */
    void drawGrid (juce::Graphics& g, float width, float height);

    //==============================================================================
    // Members
    //==============================================================================

    SpectrumDataBridge& bridge_;

    juce::dsp::FFT                              fft_    { kFFTOrder };
    juce::dsp::WindowingFunction<float>         window_ { static_cast<std::size_t> (kFFTSize),
                                                          juce::dsp::WindowingFunction<float>::hann };

    /** Working FFT buffer (2× size for real FFT padding). */
    std::array<float, kFFTSize * 2>     fftWorkBuf_   {};

    /** Smoothed output bins for input signal (GUI thread only). */
    std::array<float, kFFTSize / 2>     inputSmoothed_  {};

    /** Smoothed output bins for output signal (GUI thread only). */
    std::array<float, kFFTSize / 2>     outputSmoothed_ {};

    /** Staging buffers — filled from bridge then passed to processFFT. */
    std::array<float, kFFTSize>         inputStageBuf_  {};
    std::array<float, kFFTSize>         outputStageBuf_ {};

    /** Exponential smoothing alpha [0, 1]. */
    float smoothingAlpha_ { 0.8f };

    /** Nyquist frequency (Hz), updated by setSampleRate(). */
    float maxFreqHz_ { kDefaultMaxFreqHz };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzerWidget)
};
