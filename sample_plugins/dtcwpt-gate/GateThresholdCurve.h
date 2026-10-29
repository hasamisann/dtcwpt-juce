#pragma once

/**
 * @file GateThresholdCurve.h
 * @brief Topology-independent threshold curve of the Gate plugin.
 *
 * The Gate exposes 31 threshold parameters at the ISO 1/3-octave nominal centre frequencies
 * (20 Hz ... 20 kHz). The threshold of a subband is derived from those points and the
 * frequency range of the band, so a parameter always means "the threshold around this
 * frequency" whatever the topology is.
 *
 * Definition. Let x = log10(f) and let (x_k, y_k), k = 0..30, be the 31 points (y_k in dB).
 * The curve c(x) joins the points linearly in (x, dB) and is constant outside them:
 * c(x) = y_0 for x < x_0 and c(x) = y_30 for x > x_30. For a band [lo, hi] Hz the lower edge
 * is raised to 20 Hz (lo = 0 for the lowest band), a = log10(max(lo, 20)), b = log10(max(hi, 20)),
 * and the band threshold is the mean of the curve over the band in log frequency:
 *
 *     T_dB = 1 / (b - a) * integral_a^b c(x) dx           (c(a) when b <= a)
 *
 * The integral is exact because c is piecewise linear (trapezoids between the breakpoints
 * inside [a, b]). The linear threshold is 10^(T_dB / 20).
 *
 * Header-only, allocation-free and independent of JUCE so that it is usable on the audio thread.
 */

#include <algorithm>
#include <array>
#include <cmath>

namespace gate_threshold
{
    inline constexpr int kNumPoints = 31;

    inline constexpr float kMinDb = -120.0f;
    inline constexpr float kMaxDb = 10.0f;
    inline constexpr float kDefaultDb = -60.0f;

    struct PointInfo
    {
        double frequencyHz;
        const char* idSuffix; ///< Appended to "threshold_" to form the parameter ID.
        const char* label;    ///< Appended to "Threshold " to form the parameter name.
    };

    inline constexpr std::array<PointInfo, kNumPoints> kPoints {{
        {    20.0, "20hz",     "20 Hz"     },
        {    25.0, "25hz",     "25 Hz"     },
        {    31.5, "31_5hz",   "31.5 Hz"   },
        {    40.0, "40hz",     "40 Hz"     },
        {    50.0, "50hz",     "50 Hz"     },
        {    63.0, "63hz",     "63 Hz"     },
        {    80.0, "80hz",     "80 Hz"     },
        {   100.0, "100hz",    "100 Hz"    },
        {   125.0, "125hz",    "125 Hz"    },
        {   160.0, "160hz",    "160 Hz"    },
        {   200.0, "200hz",    "200 Hz"    },
        {   250.0, "250hz",    "250 Hz"    },
        {   315.0, "315hz",    "315 Hz"    },
        {   400.0, "400hz",    "400 Hz"    },
        {   500.0, "500hz",    "500 Hz"    },
        {   630.0, "630hz",    "630 Hz"    },
        {   800.0, "800hz",    "800 Hz"    },
        {  1000.0, "1khz",     "1 kHz"     },
        {  1250.0, "1_25khz",  "1.25 kHz"  },
        {  1600.0, "1_6khz",   "1.6 kHz"   },
        {  2000.0, "2khz",     "2 kHz"     },
        {  2500.0, "2_5khz",   "2.5 kHz"   },
        {  3150.0, "3_15khz",  "3.15 kHz"  },
        {  4000.0, "4khz",     "4 kHz"     },
        {  5000.0, "5khz",     "5 kHz"     },
        {  6300.0, "6_3khz",   "6.3 kHz"   },
        {  8000.0, "8khz",     "8 kHz"     },
        { 10000.0, "10khz",    "10 kHz"    },
        { 12500.0, "12_5khz",  "12.5 kHz"  },
        { 16000.0, "16khz",    "16 kHz"    },
        { 20000.0, "20khz",    "20 kHz"    },
    }};

    inline constexpr double kMinFrequencyHz = kPoints.front().frequencyHz;

    /** log10 of the point frequencies, computed once. */
    inline const std::array<double, kNumPoints>& getLogFrequencies() noexcept
    {
        static const std::array<double, kNumPoints> table = []
        {
            std::array<double, kNumPoints> logs {};
            for (std::size_t k = 0; k < logs.size(); ++k)
                logs[k] = std::log10 (kPoints[k].frequencyHz);
            return logs;
        }();
        return table;
    }

    /** Curve value c(x) in dB at x = log10(frequency). */
    inline double evaluateDbAtLogFrequency (const double* pointsDb, double x) noexcept
    {
        const auto& logs = getLogFrequencies();

        if (x <= logs.front())
            return pointsDb[0];
        if (x >= logs.back())
            return pointsDb[kNumPoints - 1];

        std::size_t k = 0;
        while (x >= logs[k + 1])
            ++k;

        const double t = (x - logs[k]) / (logs[k + 1] - logs[k]);
        return pointsDb[k] + t * (pointsDb[k + 1] - pointsDb[k]);
    }

    /** Curve value in dB at a frequency in Hz (frequencies below 20 Hz use the 20 Hz value). */
    inline double evaluateDb (const double* pointsDb, double frequencyHz) noexcept
    {
        return evaluateDbAtLogFrequency (pointsDb, std::log10 (std::max (frequencyHz, kMinFrequencyHz)));
    }

    /**
     * @brief Threshold (dB) of the band [lowHz, highHz]: the mean of the curve over the band in log frequency.
     * @param pointsDb kNumPoints values in dB, in the order of kPoints.
     */
    inline double bandThresholdDb (const double* pointsDb, double lowHz, double highHz) noexcept
    {
        const auto& logs = getLogFrequencies();

        const double a = std::log10 (std::max (lowHz, kMinFrequencyHz));
        const double b = std::log10 (std::max (highHz, kMinFrequencyHz));

        if (! (b > a))
            return evaluateDbAtLogFrequency (pointsDb, a);

        double integral = 0.0;
        double xPrev = a;
        double yPrev = evaluateDbAtLogFrequency (pointsDb, a);

        for (std::size_t k = 0; k < logs.size(); ++k)
        {
            if (logs[k] <= a)
                continue;
            if (logs[k] >= b)
                break;

            integral += 0.5 * (yPrev + pointsDb[k]) * (logs[k] - xPrev);
            xPrev = logs[k];
            yPrev = pointsDb[k];
        }

        integral += 0.5 * (yPrev + evaluateDbAtLogFrequency (pointsDb, b)) * (b - xPrev);
        return integral / (b - a);
    }

    /** Linear amplitude of a level in dB. */
    inline double dbToLinear (double db) noexcept
    {
        return std::pow (10.0, db / 20.0);
    }
}
