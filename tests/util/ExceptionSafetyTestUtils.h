#pragma once

/**
 * @file ExceptionSafetyTestUtils.h
 * @brief Small buffer helpers shared by the exception-safety suites.
 */

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace ExceptionSafetyTestUtils
{
/** Stereo float buffer filled with a constant value. */
inline juce::AudioBuffer<float> makeConstantBuffer (int numSamples, float value)
{
    juce::AudioBuffer<float> buffer (2, numSamples);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), value, numSamples);
    return buffer;
}

/** Stereo float buffer holding a deterministic sine-plus-offset signal. */
inline juce::AudioBuffer<float> makeToneBuffer (int numSamples, int startSample)
{
    juce::AudioBuffer<float> buffer (2, numSamples);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* data = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            data[i] = 0.5f * std::sin (0.05f * static_cast<float> (startSample + i) + 0.3f * static_cast<float> (ch));
    }
    return buffer;
}

inline bool isAllZero (const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto* data = buffer.getReadPointer (ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (! juce::exactlyEqual (data[i], 0.0f))
                return false;
    }
    return true;
}

inline bool isAllFinite (const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto* data = buffer.getReadPointer (ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (! std::isfinite (data[i]))
                return false;
    }
    return true;
}

inline bool isAllFinite (const juce::AudioBuffer<double>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto* data = buffer.getReadPointer (ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (! std::isfinite (data[i]))
                return false;
    }
    return true;
}

} // namespace ExceptionSafetyTestUtils
