/**
 * @file ComplexUtilsTests.cpp
 * @brief JUCE UnitTest suite for ComplexUtils (Re/Im <-> magnitude/phase conversion).
 */

#include <dtcwpt/dtcwpt_complex_utils.h>
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <cmath>
#include <random>
#include <vector>

namespace {

constexpr double PI = 3.14159265358979323846;

} // anonymous namespace

class ComplexUtilsTests : public juce::UnitTest {
public:
    ComplexUtilsTests() : UnitTest("ComplexUtils") {}

    void runTest() override {
        beginTest("Round trip Re/Im -> mag/phase -> Re/Im");
        testRoundTrip();

        beginTest("In-place operation");
        testInPlace();

        beginTest("Zero vector handling");
        testZeroVector();

        beginTest("Known values");
        testKnownValues();

        beginTest("Boundary values around zero and phase wrapping");
        testBoundaryValuesAroundZeroAndPhaseWrapping();
    }

private:
    void testRoundTrip() {
        // Generate 100 random Re/Im pairs using fixed seed (42)
        std::mt19937 rng(TestUtils::TestConfig::RANDOM_SEED);
        std::uniform_real_distribution<double> dist(-100.0, 100.0);

        constexpr size_t N = 100;
        std::vector<double> originalRe(N);
        std::vector<double> originalIm(N);
        std::vector<double> mag(N);
        std::vector<double> phase(N);
        std::vector<double> recoveredRe(N);
        std::vector<double> recoveredIm(N);

        for (size_t i = 0; i < N; ++i) {
            originalRe[i] = dist(rng);
            originalIm[i] = dist(rng);
        }

        // Re/Im -> mag/phase
        dtcwpt::complexToMagPhase(originalRe.data(), originalIm.data(),
                                   mag.data(), phase.data(), N);

        // mag/phase -> Re/Im
        dtcwpt::magPhaseToComplex(mag.data(), phase.data(),
                                   recoveredRe.data(), recoveredIm.data(), N);

        // Verify each value matches original within STRICT_EPSILON
        for (size_t i = 0; i < N; ++i) {
            expectWithinAbsoluteError(recoveredRe[i], originalRe[i], 
                                       TestUtils::TestConfig::STRICT_EPSILON,
                                       "Re[" + juce::String(i) + "] mismatch");
            expectWithinAbsoluteError(recoveredIm[i], originalIm[i], 
                                       TestUtils::TestConfig::STRICT_EPSILON,
                                       "Im[" + juce::String(i) + "] mismatch");
        }
    }

    void testInPlace() {
        // Test in-place complexToMagPhase
        std::vector<double> dataRe = {3.0, 4.0};
        std::vector<double> dataIm = {4.0, 3.0};

        // Use same arrays for output (in-place)
        dtcwpt::complexToMagPhase(dataRe.data(), dataIm.data(),
                                   dataRe.data(), dataIm.data(), 2);

        // Expected mag: {5.0, 5.0}
        expectWithinAbsoluteError(dataRe[0], 5.0, TestUtils::TestConfig::STRICT_EPSILON,
                                   "In-place mag[0] should be 5.0");
        expectWithinAbsoluteError(dataRe[1], 5.0, TestUtils::TestConfig::STRICT_EPSILON,
                                   "In-place mag[1] should be 5.0");

        // Test in-place magPhaseToComplex
        std::vector<double> mag = {2.0, 1.0};
        std::vector<double> phase = {0.0, PI / 2.0};

        dtcwpt::magPhaseToComplex(mag.data(), phase.data(),
                                   mag.data(), phase.data(), 2);

        // Expected re: {2.0, 0.0}
        expectWithinAbsoluteError(mag[0], 2.0, TestUtils::TestConfig::STRICT_EPSILON,
                                   "In-place re[0] should be 2.0");
        expectWithinAbsoluteError(mag[1], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                   "In-place re[1] should be 0.0");

        // Expected im: {0.0, 1.0}
        expectWithinAbsoluteError(phase[0], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                   "In-place im[0] should be 0.0");
        expectWithinAbsoluteError(phase[1], 1.0, TestUtils::TestConfig::STRICT_EPSILON,
                                   "In-place im[1] should be 1.0");
    }

    void testZeroVector() {
        std::vector<double> re = {0.0, 0.0};
        std::vector<double> im = {0.0, 0.0};
        std::vector<double> mag(2);
        std::vector<double> phase(2);

        dtcwpt::complexToMagPhase(re.data(), im.data(), mag.data(), phase.data(), 2);

        // mag should be 0.0
        for (size_t i = 0; i < 2; ++i) {
            expectWithinAbsoluteError(mag[i], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                       "mag[" + juce::String(i) + "] should be 0.0");
        }

        // Round-trip should preserve zeros
        std::vector<double> recoveredRe(2);
        std::vector<double> recoveredIm(2);
        dtcwpt::magPhaseToComplex(mag.data(), phase.data(),
                                   recoveredRe.data(), recoveredIm.data(), 2);

        for (size_t i = 0; i < 2; ++i) {
            expectWithinAbsoluteError(recoveredRe[i], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                       "Round-trip re[" + juce::String(i) + "] should be 0.0");
            expectWithinAbsoluteError(recoveredIm[i], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                       "Round-trip im[" + juce::String(i) + "] should be 0.0");
        }
    }

    void testKnownValues() {
        // Input: re={3.0, 0.0, -1.0}, im={4.0, 1.0, 0.0}
        std::vector<double> re = {3.0, 0.0, -1.0};
        std::vector<double> im = {4.0, 1.0, 0.0};
        std::vector<double> mag(3);
        std::vector<double> phase(3);

        dtcwpt::complexToMagPhase(re.data(), im.data(), mag.data(), phase.data(), 3);

        // Expected mag: {5.0, 1.0, 1.0}
        expectWithinAbsoluteError(mag[0], 5.0, TestUtils::TestConfig::STRICT_EPSILON,
                                   "mag[0] should be 5.0 (3,4,5 triangle)");
        expectWithinAbsoluteError(mag[1], 1.0, TestUtils::TestConfig::STRICT_EPSILON,
                                   "mag[1] should be 1.0");
        expectWithinAbsoluteError(mag[2], 1.0, TestUtils::TestConfig::STRICT_EPSILON,
                                   "mag[2] should be 1.0");

        // Expected phase: {atan2(4,3), pi/2, pi}
        expectWithinAbsoluteError(phase[0], std::atan2(4.0, 3.0), TestUtils::TestConfig::STRICT_EPSILON,
                                   "phase[0] should be atan2(4,3)");
        expectWithinAbsoluteError(phase[1], PI / 2.0, TestUtils::TestConfig::STRICT_EPSILON,
                                   "phase[1] should be pi/2");
        expectWithinAbsoluteError(phase[2], PI, TestUtils::TestConfig::STRICT_EPSILON,
                                   "phase[2] should be pi");
    }

    void testBoundaryValuesAroundZeroAndPhaseWrapping() {
        const std::vector<double> magnitudes = {0.0, 1.0e-15, 1.0, 1.0, 1.0, 1.0};
        const std::vector<double> phases = {0.0, PI, PI, -PI, 2.0 * PI, -2.0 * PI};
        std::vector<double> re(magnitudes.size(), 0.0);
        std::vector<double> im(magnitudes.size(), 0.0);

        dtcwpt::magPhaseToComplex(magnitudes.data(), phases.data(), re.data(), im.data(), magnitudes.size());

        expectWithinAbsoluteError(re[0], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                  "Zero magnitude should stay at zero real part");
        expectWithinAbsoluteError(im[0], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                  "Zero magnitude should stay at zero imaginary part");
        expect(re[1] < 0.0, "Tiny negative-axis magnitude should preserve sign around pi");
        expectWithinAbsoluteError(im[1], 0.0, 1.0e-20,
                                  "Tiny negative-axis magnitude should stay close to zero imaginary part");
        expectWithinAbsoluteError(re[2], -1.0, TestUtils::TestConfig::STRICT_EPSILON,
                                  "Phase +pi should map to the negative real axis");
        expectWithinAbsoluteError(im[2], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                  "Phase +pi should have zero imaginary part");
        expectWithinAbsoluteError(re[3], -1.0, TestUtils::TestConfig::STRICT_EPSILON,
                                  "Phase -pi should map to the same negative real axis");
        expectWithinAbsoluteError(im[3], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                  "Phase -pi should have zero imaginary part");
        expectWithinAbsoluteError(re[4], 1.0, TestUtils::TestConfig::STRICT_EPSILON,
                                  "Phase +2pi should wrap to the positive real axis");
        expectWithinAbsoluteError(im[4], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                  "Phase +2pi should wrap with zero imaginary part");
        expectWithinAbsoluteError(re[5], 1.0, TestUtils::TestConfig::STRICT_EPSILON,
                                  "Phase -2pi should wrap to the positive real axis");
        expectWithinAbsoluteError(im[5], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                  "Phase -2pi should wrap with zero imaginary part");

        std::vector<double> roundTripMagnitude(magnitudes.size(), 0.0);
        std::vector<double> roundTripPhase(magnitudes.size(), 0.0);
        dtcwpt::complexToMagPhase(re.data(), im.data(), roundTripMagnitude.data(), roundTripPhase.data(), re.size());

        for (size_t i = 0; i < magnitudes.size(); ++i) {
            expectWithinAbsoluteError(roundTripMagnitude[i], magnitudes[i], 1.0e-12,
                                      "Magnitude should survive wrapped roundtrip at index " + juce::String(static_cast<int>(i)));
        }

        expectWithinAbsoluteError(roundTripPhase[0], 0.0, TestUtils::TestConfig::STRICT_EPSILON,
                                  "Zero vector phase should remain pinned at zero");
        expectWithinAbsoluteError(std::abs(roundTripPhase[1]), PI, 1.0e-12,
                                  "Tiny negative-axis phase should land on the pi boundary");
        expectWithinAbsoluteError(std::abs(roundTripPhase[2]), PI, 1.0e-12,
                                  "Positive pi should remain on the wrapped boundary");
        expectWithinAbsoluteError(std::abs(roundTripPhase[3]), PI, 1.0e-12,
                                  "Negative pi should remain on the wrapped boundary");
        expectWithinAbsoluteError(roundTripPhase[4], 0.0, 1.0e-12,
                                  "Positive 2pi should wrap back to zero phase");
        expectWithinAbsoluteError(roundTripPhase[5], 0.0, 1.0e-12,
                                  "Negative 2pi should wrap back to zero phase");
    }
};

// Static registration
static ComplexUtilsTests complexUtilsTests;
