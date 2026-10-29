#include "GateThresholdCurve.h"
#include "../../sample_plugins/dtcwpt-gate/gui/GateGuiFrequencyMapping.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <set>
#include <string>

class GateThresholdCurveTests : public juce::UnitTest
{
public:
    GateThresholdCurveTests() : juce::UnitTest ("GateThresholdCurveTests") {}

    void runTest() override
    {
        beginTest ("Point table is the 31 ISO 1/3-octave nominal frequencies with unique IDs");
        testPointTable();

        beginTest ("Equal points give the same threshold for any band");
        testConstantCurve();

        beginTest ("A band equal to one segment gets the mean of its two points");
        testBandEqualToSegment();

        beginTest ("The mean is taken in log frequency, not in Hz");
        testMeanIsInLogFrequency();

        beginTest ("A band spanning several segments is the exact trapezoid sum");
        testBandSpanningSegments();

        beginTest ("Band edges outside 20 Hz .. 20 kHz");
        testEdgesOutsideRange();

        beginTest ("Curve evaluation interpolates in log frequency and is constant outside the points");
        testEvaluate();

        beginTest ("dB to linear conversion");
        testDbToLinear();

        beginTest ("Nearest threshold point is chosen on the log axis");
        testNearestThresholdPoint();
    }

private:
    using Points = std::array<double, gate_threshold::kNumPoints>;

    static Points makePoints (double value)
    {
        Points points;
        points.fill (value);
        return points;
    }

    static int indexOf (double frequencyHz)
    {
        for (int k = 0; k < gate_threshold::kNumPoints; ++k)
            if (gate_threshold::kPoints[static_cast<std::size_t> (k)].frequencyHz == frequencyHz)
                return k;
        return -1;
    }

    void testPointTable()
    {
        const double expected[] = { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630,
                                    800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000,
                                    12500, 16000, 20000 };
        expectEquals (gate_threshold::kNumPoints, 31);

        std::set<std::string> ids;
        for (int k = 0; k < gate_threshold::kNumPoints; ++k)
        {
            const auto& point = gate_threshold::kPoints[static_cast<std::size_t> (k)];
            expectEquals (point.frequencyHz, expected[k]);
            if (k > 0)
                expect (point.frequencyHz > gate_threshold::kPoints[static_cast<std::size_t> (k - 1)].frequencyHz,
                        "Points must be strictly increasing in frequency");
            ids.insert (point.idSuffix);
        }
        expectEquals (static_cast<int> (ids.size()), 31, "Parameter ID suffixes must be unique");
        expectEquals (juce::String (gate_threshold::kPoints[static_cast<std::size_t> (indexOf (1000.0))].idSuffix), juce::String ("1khz"));
        expectEquals (juce::String (gate_threshold::kPoints[static_cast<std::size_t> (indexOf (1000.0))].label), juce::String ("1 kHz"));
        expectEquals (juce::String (gate_threshold::kPoints[static_cast<std::size_t> (indexOf (31.5))].idSuffix), juce::String ("31_5hz"));
    }

    void testConstantCurve()
    {
        const auto points = makePoints (-33.0);
        const double bands[][2] = { { 0.0, 100.0 }, { 100.0, 125.0 }, { 345.0, 689.0 }, { 689.0, 22050.0 }, { 0.0, 22050.0 } };

        for (const auto& band : bands)
            expectWithinAbsoluteError (gate_threshold::bandThresholdDb (points.data(), band[0], band[1]), -33.0, 1.0e-12);
    }

    void testBandEqualToSegment()
    {
        auto points = makePoints (-60.0);
        points[static_cast<std::size_t> (indexOf (100.0))] = -100.0;
        points[static_cast<std::size_t> (indexOf (125.0))] = -40.0;

        expectWithinAbsoluteError (gate_threshold::bandThresholdDb (points.data(), 100.0, 125.0), -70.0, 1.0e-9);
    }

    void testMeanIsInLogFrequency()
    {
        auto points = makePoints (-60.0);
        points[static_cast<std::size_t> (indexOf (100.0))] = -100.0;
        points[static_cast<std::size_t> (indexOf (125.0))] = -40.0;

        // The geometric mean of 100 and 125 Hz is the middle of the segment on the log axis (value -70 dB);
        // the first half then has the mean of -100 and -70.
        const double geometricMean = std::sqrt (100.0 * 125.0);
        expectWithinAbsoluteError (gate_threshold::bandThresholdDb (points.data(), 100.0, geometricMean), -85.0, 1.0e-9);
        expectWithinAbsoluteError (gate_threshold::bandThresholdDb (points.data(), geometricMean, 125.0), -55.0, 1.0e-9);
        // Taking the middle in Hz (112.5 Hz) would give a different value for the lower half.
        expect (std::abs (gate_threshold::bandThresholdDb (points.data(), 100.0, 112.5) - -85.0) > 0.1);
    }

    void testBandSpanningSegments()
    {
        auto points = makePoints (-40.0);
        points[static_cast<std::size_t> (indexOf (100.0))] = -100.0;

        // 100 Hz (-100) -> 125 Hz (-40) -> 160 Hz (-40)
        const double lenA = std::log10 (125.0 / 100.0);
        const double lenB = std::log10 (160.0 / 125.0);
        const double expected = (-70.0 * lenA + -40.0 * lenB) / (lenA + lenB);

        expectWithinAbsoluteError (gate_threshold::bandThresholdDb (points.data(), 100.0, 160.0), expected, 1.0e-9);

        // A band that starts and ends inside different segments: 110 .. 140 Hz.
        const double t110 = std::log10 (110.0 / 100.0) / std::log10 (125.0 / 100.0);
        const double y110 = -100.0 + 60.0 * t110;
        const double partA = 0.5 * (y110 + -40.0) * std::log10 (125.0 / 110.0);
        const double partB = -40.0 * std::log10 (140.0 / 125.0);
        expectWithinAbsoluteError (gate_threshold::bandThresholdDb (points.data(), 110.0, 140.0),
                                   (partA + partB) / std::log10 (140.0 / 110.0), 1.0e-9);
    }

    void testEdgesOutsideRange()
    {
        auto points = makePoints (-60.0);
        points.front() = -20.0;
        points.back() = -90.0;

        // lo = 0 is treated as 20 Hz.
        expectWithinAbsoluteError (gate_threshold::bandThresholdDb (points.data(), 0.0, 40.0),
                                   gate_threshold::bandThresholdDb (points.data(), 20.0, 40.0), 1.0e-12);
        // A band completely below 20 Hz has the 20 Hz value; so does an empty band.
        expectWithinAbsoluteError (gate_threshold::bandThresholdDb (points.data(), 0.0, 10.0), -20.0, 1.0e-12);
        expectWithinAbsoluteError (gate_threshold::bandThresholdDb (points.data(), 50.0, 50.0), gate_threshold::evaluateDb (points.data(), 50.0), 1.0e-12);
        // A band completely above 20 kHz has the 20 kHz value.
        expectWithinAbsoluteError (gate_threshold::bandThresholdDb (points.data(), 24000.0, 48000.0), -90.0, 1.0e-12);
        // A band crossing 20 kHz: the part above 20 kHz is constant (-90 dB).
        const double lenBelow = std::log10 (20000.0 / 16000.0);
        const double lenAbove = std::log10 (24000.0 / 20000.0);
        expectWithinAbsoluteError (gate_threshold::bandThresholdDb (points.data(), 16000.0, 24000.0),
                                   (0.5 * (-60.0 + -90.0) * lenBelow + -90.0 * lenAbove) / (lenBelow + lenAbove), 1.0e-9);
    }

    void testEvaluate()
    {
        auto points = makePoints (-60.0);
        points[static_cast<std::size_t> (indexOf (1000.0))] = -20.0;

        expectWithinAbsoluteError (gate_threshold::evaluateDb (points.data(), 1000.0), -20.0, 1.0e-12);
        expectWithinAbsoluteError (gate_threshold::evaluateDb (points.data(), std::sqrt (800.0 * 1000.0)), -40.0, 1.0e-9);
        expectWithinAbsoluteError (gate_threshold::evaluateDb (points.data(), 5.0), -60.0, 1.0e-12);
        expectWithinAbsoluteError (gate_threshold::evaluateDb (points.data(), 30000.0), -60.0, 1.0e-12);
    }

    void testDbToLinear()
    {
        expectWithinAbsoluteError (gate_threshold::dbToLinear (0.0), 1.0, 1.0e-15);
        expectWithinAbsoluteError (gate_threshold::dbToLinear (-20.0), 0.1, 1.0e-15);
        expectWithinAbsoluteError (gate_threshold::dbToLinear (-120.0), 1.0e-6, 1.0e-18);
    }

    void testNearestThresholdPoint()
    {
        const float nyquist = 22050.0f;
        expectEquals (gate_gui::findNearestThresholdPoint (1000.0f, nyquist), indexOf (1000.0));
        expectEquals (gate_gui::findNearestThresholdPoint (1.0f, nyquist), 0);
        expectEquals (gate_gui::findNearestThresholdPoint (30000.0f, nyquist), gate_threshold::kNumPoints - 1);
        // 1120 Hz is closer to 1000 Hz than to 1250 Hz in log distance (sqrt(1000*1250) = 1118 Hz).
        expectEquals (gate_gui::findNearestThresholdPoint (1117.0f, nyquist), indexOf (1000.0));
        expectEquals (gate_gui::findNearestThresholdPoint (1120.0f, nyquist), indexOf (1250.0));

        // Points above the Nyquist frequency are not offered.
        expectEquals (gate_gui::countThresholdPointsBelow (22050.0f), 31);
        expectEquals (gate_gui::countThresholdPointsBelow (12000.0f), indexOf (10000.0) + 1);
        expectEquals (gate_gui::findNearestThresholdPoint (11900.0f, 12000.0f), indexOf (10000.0));
    }
};

static GateThresholdCurveTests gateThresholdCurveTests;
