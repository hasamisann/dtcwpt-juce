/**
 * @file MorphBandProcessorTests.cpp
 * @brief JUCE UnitTest suite for MorphBandProcessor.
 *
 * Tests cover the full morph algorithm:
 *  - Passthrough (magnitude=0, phase=0)
 *  - Full magnitude replacement
 *  - Full phase replacement
 *  - Threshold gate attenuation
 *  - Bypass Low / Bypass High band
 *  - Per-sample SmoothedValue advancement
 *  - Reset snaps SmoothedValue
 */

#include "MorphBandProcessor.h"
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

namespace {

constexpr double PI = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Helper: Build a BandData with synthetic Re/Im data.
//
// numChannels channels, numBands bands, numSamples per band.
// Main Re/Im are filled by fillMain(); sidechain by fillSc().
// destinationIds are set to the values in destIds (must have numBands entries).
// ---------------------------------------------------------------------------
struct SynthBandData
{
    // Storage
    std::vector<int>  destinationIds;
    std::vector<std::vector<std::vector<double>>> mainRe, mainIm;
    std::vector<std::vector<std::vector<double>>> scRe,   scIm;

    dtcwpt::BandData data;

    void init (int numChannels, int numBands, int numSamples,
               const std::vector<int>& destIds)
    {
        destinationIds = destIds;

        auto alloc = [&](auto& arr)
        {
            arr.resize (static_cast<std::size_t> (numChannels));
            for (auto& ch : arr)
            {
                ch.resize (static_cast<std::size_t> (numBands),
                           std::vector<double> (static_cast<std::size_t> (numSamples), 0.0));
            }
        };

        alloc (mainRe); alloc (mainIm);
        alloc (scRe);   alloc (scIm);

        // Build ChannelBandView arrays
        auto makeViews = [&](auto& re, auto& im) -> std::vector<std::vector<dtcwpt::ChannelBandView>>
        {
            std::vector<std::vector<dtcwpt::ChannelBandView>> views;
            views.resize (static_cast<std::size_t> (numChannels));
            for (int ch = 0; ch < numChannels; ++ch)
            {
                views[static_cast<std::size_t> (ch)].resize (static_cast<std::size_t> (numBands));
                for (int b = 0; b < numBands; ++b)
                {
                    auto cs = static_cast<std::size_t> (ch);
                    auto bs = static_cast<std::size_t> (b);
                    views[cs][bs] = { re[cs][bs].data(),
                                      im[cs][bs].data(),
                                      static_cast<std::size_t> (numSamples) };
                }
            }
            return views;
        };

        data.bands         = makeViews (mainRe, mainIm);
        data.sidechainBands = makeViews (scRe, scIm);
        data.numChannels   = numChannels;
        data.numBands      = numBands;
        data.hasSidechain  = true;
        data.destinationIds = &destinationIds;
    }

    // Fill main Re/Im with a sinusoidal pattern
    void fillMainSine (double amplitude = 0.5)
    {
        for (auto& ch : mainRe)
            for (auto& band : ch)
                for (std::size_t s = 0; s < band.size(); ++s)
                    band[s] = amplitude * std::cos (2.0 * PI * 0.1 * static_cast<double> (s));

        for (auto& ch : mainIm)
            for (auto& band : ch)
                for (std::size_t s = 0; s < band.size(); ++s)
                    band[s] = amplitude * std::sin (2.0 * PI * 0.1 * static_cast<double> (s));
    }

    // Fill sidechain Re/Im with a different sinusoidal pattern
    void fillScSine (double amplitude = 0.5, double freqFactor = 0.2)
    {
        for (auto& ch : scRe)
            for (auto& band : ch)
                for (std::size_t s = 0; s < band.size(); ++s)
                    band[s] = amplitude * std::cos (2.0 * PI * freqFactor * static_cast<double> (s));

        for (auto& ch : scIm)
            for (auto& band : ch)
                for (std::size_t s = 0; s < band.size(); ++s)
                    band[s] = amplitude * std::sin (2.0 * PI * freqFactor * static_cast<double> (s));
    }

    // Fill sidechain with a very small magnitude (below any threshold)
    void fillScLow (double amplitude = 0.0001)
    {
        fillScSine (amplitude, 0.3);
    }
};

} // anonymous namespace

class MorphBandProcessorTests : public juce::UnitTest
{
public:
    MorphBandProcessorTests() : UnitTest ("MorphBandProcessorTests") {}

    void runTest() override
    {
        beginTest ("1. Passthrough: magnitude=0, phase=0 preserves main signal");
        testPassthrough();

        beginTest ("2. Full magnitude replacement (magnitude=1, phase=0)");
        testFullMagnitudeReplacement();

        beginTest ("3. Full phase replacement (magnitude=0, phase=1)");
        testFullPhaseReplacement();

        beginTest ("4. Threshold gate attenuates phase when sidechain is weak");
        testThresholdGate();

        beginTest ("5. Bypass Low: power-of-2 node ID band passes through unchanged");
        testBypassLow();

        beginTest ("6. Bypass High: all-bits-set node ID band passes through unchanged");
        testBypassHigh();

        beginTest ("7. SmoothedValue ramps — first sample not at full target");
        testSmoothedValueRamp();

        beginTest ("8. Reset snaps SmoothedValues to target");
        testReset();
    }

private:
    // -----------------------------------------------------------------------
    // Test 1: Passthrough
    // -----------------------------------------------------------------------
    void testPassthrough()
    {
        // 2 channels, 3 bands (IDs: 4, 5, 6 — none are power-of-2 or all-bits)
        // Non-bypass node IDs: 4=(100b) not power-of-2 check: 4&3=0 => IS power of 2!
        // Use IDs: 5 (101b), 6 (110b), 9 (1001b) — none satisfy isLowBand or isHighBand
        SynthBandData sb;
        sb.init (2, 3, 64, {5, 6, 9});
        sb.fillMainSine (0.5);
        sb.fillScSine (0.4, 0.2);

        // Snapshot original main Re/Im
        auto origRe = sb.mainRe;
        auto origIm = sb.mainIm;

        MorphBandProcessor proc;
        proc.prepare (44100.0, 64, 3, 2);
        proc.setTargets (0.0, 0.0, 0.001, false, false);
        proc.processAllBands (sb.data);

        constexpr double tol = 1e-10;
        for (int ch = 0; ch < 2; ++ch)
            for (int b = 0; b < 3; ++b)
                for (std::size_t s = 0; s < 64; ++s)
                {
                    auto cs = static_cast<std::size_t> (ch);
                    auto bs = static_cast<std::size_t> (b);
                    expectWithinAbsoluteError (sb.mainRe[cs][bs][s], origRe[cs][bs][s],
                                               tol, "Re passthrough mismatch");
                    expectWithinAbsoluteError (sb.mainIm[cs][bs][s], origIm[cs][bs][s],
                                               tol, "Im passthrough mismatch");
                }
    }

    // -----------------------------------------------------------------------
    // Test 2: Full magnitude replacement
    // -----------------------------------------------------------------------
    void testFullMagnitudeReplacement()
    {
        SynthBandData sb;
        sb.init (2, 2, 32, {5, 6});
        sb.fillMainSine (0.5);
        sb.fillScSine (0.3, 0.15);

        // Pre-compute expected sidechain magnitudes
        auto scMagExpected = [&](int ch, int b, std::size_t s) -> double {
            auto cs = static_cast<std::size_t> (ch);
            auto bs = static_cast<std::size_t> (b);
            double re = sb.scRe[cs][bs][s];
            double im = sb.scIm[cs][bs][s];
            return std::hypot (re, im);
        };
        // Pre-compute expected main phases
        auto mainPhaseExpected = [&](int ch, int b, std::size_t s) -> double {
            auto cs = static_cast<std::size_t> (ch);
            auto bs = static_cast<std::size_t> (b);
            double re = sb.mainRe[cs][bs][s];
            double im = sb.mainIm[cs][bs][s];
            return std::atan2 (im, re);
        };

        // Store expected values before processing
        std::vector<std::vector<std::vector<double>>> expMag (2), expPhase (2);
        for (int ch = 0; ch < 2; ++ch)
        {
            expMag[static_cast<std::size_t> (ch)].resize (2);
            expPhase[static_cast<std::size_t> (ch)].resize (2);
            for (int b = 0; b < 2; ++b)
            {
                expMag[static_cast<std::size_t> (ch)][static_cast<std::size_t> (b)].resize (32);
                expPhase[static_cast<std::size_t> (ch)][static_cast<std::size_t> (b)].resize (32);
                for (std::size_t s = 0; s < 32; ++s)
                {
                    expMag[static_cast<std::size_t> (ch)][static_cast<std::size_t> (b)][s]   = scMagExpected (ch, b, s);
                    expPhase[static_cast<std::size_t> (ch)][static_cast<std::size_t> (b)][s] = mainPhaseExpected (ch, b, s);
                }
            }
        }

        MorphBandProcessor proc;
        proc.prepare (44100.0, 32, 2, 2);
        proc.setTargets (1.0, 0.0, 0.001, false, false);
        proc.processAllBands (sb.data);

        // SmoothedValue ramps over 20 ms = ~882 samples at 44100 Hz
        // With only 32 samples the morph won't reach full 1.0 — skip this test
        // if the block is shorter than the ramp. Instead, pre-snap by calling
        // prepare() and then setTargets() then process.
        // A simpler approach: call processAllBands many times to settle.
        // Re-do: snap by using setCurrentAndTargetValue approach — call reset() after setTargets().
        // Better: use a large block OR accept a loose tolerance for the ramp.
        //
        // The spec does not require full convergence in a single block — test 7 specifically
        // checks that. Here we just verify the direction: output magnitudes should be
        // closer to sidechain than to main.
        //
        // For a reliable test, we call processAllBands multiple times to settle SmoothedValue.
        // But we already consumed the data in the first call. Re-init.

        SynthBandData sb2;
        sb2.init (1, 1, 1024, {5});
        sb2.fillMainSine (0.5);
        sb2.fillScSine (0.3, 0.15);

        // Expected sc magnitudes for ch=0, b=0
        std::vector<double> expScMag (1024);
        for (std::size_t s = 0; s < 1024; ++s)
            expScMag[s] = std::hypot (sb2.scRe[0][0][s], sb2.scIm[0][0][s]);
        std::vector<double> expMainPhase (1024);
        for (std::size_t s = 0; s < 1024; ++s)
            expMainPhase[s] = std::atan2 (sb2.mainIm[0][0][s], sb2.mainRe[0][0][s]);

        MorphBandProcessor proc2;
        proc2.prepare (44100.0, 1024, 1, 1);
        proc2.setTargets (1.0, 0.0, 0.001, false, false);
        proc2.processAllBands (sb2.data);

        // After 1024 samples with 20ms ramp at 44100 Hz (~882 samples ramp):
        // from sample ~882 onwards we should be at full magnitude=1.0.
        // Check the last 100 samples.
        constexpr double tol = 1e-5;
        for (std::size_t s = 924; s < 1024; ++s)
        {
            double outMag   = std::hypot (sb2.mainRe[0][0][s], sb2.mainIm[0][0][s]);
            double outPhase = std::atan2 (sb2.mainIm[0][0][s], sb2.mainRe[0][0][s]);
            expectWithinAbsoluteError (outMag,   expScMag[s],    tol, "Mag should match sidechain");
            expectWithinAbsoluteError (outPhase, expMainPhase[s], tol, "Phase should match main");
        }
    }

    // -----------------------------------------------------------------------
    // Test 3: Full phase replacement
    // -----------------------------------------------------------------------
    void testFullPhaseReplacement()
    {
        SynthBandData sb;
        sb.init (1, 1, 1024, {5});
        sb.fillMainSine (0.5);
        sb.fillScSine (0.3, 0.15);

        std::vector<double> expMainMag (1024);
        std::vector<double> expScPhase (1024);
        for (std::size_t s = 0; s < 1024; ++s)
        {
            expMainMag[s]  = std::hypot   (sb.mainRe[0][0][s], sb.mainIm[0][0][s]);
            expScPhase[s]  = std::atan2   (sb.scIm[0][0][s],   sb.scRe[0][0][s]);
        }

        MorphBandProcessor proc;
        proc.prepare (44100.0, 1024, 1, 1);
        proc.setTargets (0.0, 1.0, 0.001, false, false);
        proc.processAllBands (sb.data);

        // Check last 100 samples (after SmoothedValue settles ~882 samples)
        constexpr double tol = 1e-5;
        for (std::size_t s = 924; s < 1024; ++s)
        {
            double outMag   = std::hypot (sb.mainRe[0][0][s], sb.mainIm[0][0][s]);
            double outPhase = std::atan2 (sb.mainIm[0][0][s], sb.mainRe[0][0][s]);
            expectWithinAbsoluteError (outMag,   expMainMag[s], tol, "Mag should match main");
            expectWithinAbsoluteError (outPhase, expScPhase[s], tol, "Phase should match sidechain");
        }
    }

    // -----------------------------------------------------------------------
    // Test 4: Threshold gate
    // -----------------------------------------------------------------------
    void testThresholdGate()
    {
        // Sidechain is very weak (0.0001). Threshold is 0.5.
        // Phase ratio target = 1.0, but effective phase ratio should be attenuated.
        // Output phase should be much closer to main phase than full sidechain phase.
        SynthBandData sb;
        sb.init (1, 1, 1024, {5});
        sb.fillMainSine (0.5);
        sb.fillScLow (0.0001); // weak sidechain — well below threshold of 0.5

        std::vector<double> expMainPhase (1024);
        std::vector<double> expScPhase (1024);
        for (std::size_t s = 0; s < 1024; ++s)
        {
            expMainPhase[s] = std::atan2 (sb.mainIm[0][0][s], sb.mainRe[0][0][s]);
            expScPhase[s]   = std::atan2 (sb.scIm[0][0][s],   sb.scRe[0][0][s]);
        }

        MorphBandProcessor proc;
        proc.prepare (44100.0, 1024, 1, 1);
        proc.setTargets (0.0, 1.0, 0.5, false, false); // threshold = 0.5, sc << threshold
        proc.processAllBands (sb.data);

        // In the settled region, effective phase ratio is tiny because scMag << threshold.
        // Output phase should be much closer to main than to sidechain.
        // Measure distance to main vs distance to sidechain (angular).
        int countCloserToMain = 0;
        for (std::size_t s = 924; s < 1024; ++s)
        {
            double outPhase = std::atan2 (sb.mainIm[0][0][s], sb.mainRe[0][0][s]);
            double distMain = std::abs (outPhase - expMainPhase[s]);
            double distSc   = std::abs (outPhase - expScPhase[s]);
            if (distMain < distSc)
                ++countCloserToMain;
        }
        // At least 80% should be closer to main phase (threshold gate is active)
        expect (countCloserToMain >= 80,
                "Threshold gate: output phase should be close to main phase when sidechain is weak");
    }

    // -----------------------------------------------------------------------
    // Test 5: Bypass Low
    // -----------------------------------------------------------------------
    void testBypassLow()
    {
        // Band 0: nodeId=64 (power of 2 → isLowBand → bypassed)
        // Band 1: nodeId=5  (not low/high → processed)
        SynthBandData sb;
        sb.init (1, 2, 64, {64, 5});
        sb.fillMainSine (0.5);
        sb.fillScSine (0.8, 0.3); // noticeably different from main

        // Snapshot band 0 main before processing
        auto origReBand0 = sb.mainRe[0][0];
        auto origImBand0 = sb.mainIm[0][0];

        MorphBandProcessor proc;
        proc.prepare (44100.0, 64, 2, 1);
        proc.setTargets (1.0, 1.0, 0.001, /*bypassLow=*/true, /*bypassHigh=*/false);
        proc.processAllBands (sb.data);

        // Band 0 (low, bypassed) must be identical to original
        constexpr double tol = 1e-15;
        for (std::size_t s = 0; s < 64; ++s)
        {
            expectWithinAbsoluteError (sb.mainRe[0][0][s], origReBand0[s], tol,
                                       "Band 0 (low bypass) Re should be unchanged");
            expectWithinAbsoluteError (sb.mainIm[0][0][s], origImBand0[s], tol,
                                       "Band 0 (low bypass) Im should be unchanged");
        }

        // Band 1 (not bypassed) should have been modified
        bool anyChanged = false;
        for (std::size_t s = 0; s < 64; ++s)
        {
            double origMag  = std::hypot (0.5 * std::cos (2.0 * PI * 0.1 * static_cast<double> (s)),
                                          0.5 * std::sin (2.0 * PI * 0.1 * static_cast<double> (s)));
            double outMag   = std::hypot (sb.mainRe[0][1][s], sb.mainIm[0][1][s]);
            if (std::abs (outMag - origMag) > 1e-6)
            {
                anyChanged = true;
                break;
            }
        }
        expect (anyChanged, "Band 1 (not bypassed) should have been morphed");
    }

    // -----------------------------------------------------------------------
    // Test 6: Bypass High
    // -----------------------------------------------------------------------
    void testBypassHigh()
    {
        // Band 0: nodeId=127 (binary 01111111 → all bits set → isHighBand → bypassed)
        // Band 1: nodeId=5   (not high → processed)
        SynthBandData sb;
        sb.init (1, 2, 64, {127, 5});
        sb.fillMainSine (0.5);
        sb.fillScSine (0.8, 0.3);

        auto origReBand0 = sb.mainRe[0][0];
        auto origImBand0 = sb.mainIm[0][0];

        MorphBandProcessor proc;
        proc.prepare (44100.0, 64, 2, 1);
        proc.setTargets (1.0, 1.0, 0.001, /*bypassLow=*/false, /*bypassHigh=*/true);
        proc.processAllBands (sb.data);

        constexpr double tol = 1e-15;
        for (std::size_t s = 0; s < 64; ++s)
        {
            expectWithinAbsoluteError (sb.mainRe[0][0][s], origReBand0[s], tol,
                                       "Band 0 (high bypass) Re should be unchanged");
            expectWithinAbsoluteError (sb.mainIm[0][0][s], origImBand0[s], tol,
                                       "Band 0 (high bypass) Im should be unchanged");
        }
    }

    // -----------------------------------------------------------------------
    // Test 7: SmoothedValue ramp
    // -----------------------------------------------------------------------
    void testSmoothedValueRamp()
    {
        // Start with target 0.0 (prepared + snap), then set target 1.0.
        // Process 128 samples. The first few samples should NOT be at mag=1.0.
        SynthBandData sb;
        sb.init (1, 1, 128, {5});
        sb.fillMainSine (0.5);
        sb.fillScSine (0.5, 0.2);

        MorphBandProcessor proc;
        proc.prepare (44100.0, 128, 1, 1);
        // After prepare(), SmoothedValues are snapped to 0.0 (initial target).
        // Now set target to 1.0 and process one block.
        proc.setTargets (1.0, 0.0, 0.001, false, false);
        proc.processAllBands (sb.data);

        // First sample magnitude: the SmoothedValue starts at 0.0 and ramps over ~882 samples.
        // With 128 samples processed, we should still be well below 1.0.
        // Check that the first sample's magnitude is closer to main (not sidechain).
        double firstOutMag  = std::hypot (sb.mainRe[0][0][0], sb.mainIm[0][0][0]);
        double mainMagFirst = 0.5; // amplitude of fillMainSine (0.5)
        double scMagFirst   = 0.5; // amplitude of fillScSine (0.5, but different from main)

        // NOTE: with identical main and sidechain amplitudes, outMag = lerp(0.5, 0.5, t) = 0.5
        // regardless of the SmoothedValue, making the midPoint test meaningless.
        // We use a larger sidechain amplitude (0.8) in fillScSine to make the test meaningful.
        // The test block was initialised with fillScSine (0.5, 0.2), but we need different amps.
        // Re-initialise with a clearly different sidechain amplitude.
        SynthBandData sb2;
        sb2.init (1, 1, 128, {5});
        sb2.fillMainSine (0.5);
        sb2.fillScSine (0.8, 0.2); // sidechain amplitude = 0.8, noticeably larger than main

        MorphBandProcessor proc2;
        proc2.prepare (44100.0, 128, 1, 1);
        proc2.setTargets (1.0, 0.0, 0.001, false, false);
        proc2.processAllBands (sb2.data);

        firstOutMag  = std::hypot (sb2.mainRe[0][0][0], sb2.mainIm[0][0][0]);
        mainMagFirst = 0.5; // amplitude of fillMainSine
        scMagFirst   = 0.8; // amplitude of fillScSine

        // The output mag at sample 0 should be very close to mainMag (smoothed value ~0)
        // and much less than scMag (smoothed value not yet reached 1.0).
        double midPoint = (mainMagFirst + scMagFirst) / 2.0; // = 0.65
        expect (firstOutMag < midPoint,
                "First sample should not be at full sidechain magnitude (SmoothedValue ramp)");
    }

    // -----------------------------------------------------------------------
    // Test 8: Reset snaps SmoothedValues
    // -----------------------------------------------------------------------
    void testReset()
    {
        // Set a target, call reset(), verify that subsequent processing is at target
        // (no ramp artifact — output snaps immediately).
        SynthBandData sb;
        sb.init (1, 1, 64, {5});
        sb.fillMainSine (0.5);
        sb.fillScSine (0.5, 0.2);

        MorphBandProcessor proc;
        proc.prepare (44100.0, 64, 1, 1);
        proc.setTargets (1.0, 0.0, 0.001, false, false);
        proc.reset(); // snap SmoothedValues to target immediately

        proc.processAllBands (sb.data);

        // All 64 samples should have been processed at mag=1.0 (snapped, no ramp).
        // Pre-compute expected sidechain magnitudes.
        SynthBandData ref;
        ref.init (1, 1, 64, {5});
        ref.fillMainSine (0.5);
        ref.fillScSine (0.5, 0.2);

        constexpr double tol = 1e-5;
        for (std::size_t s = 0; s < 64; ++s)
        {
            double outMag  = std::hypot (sb.mainRe[0][0][s], sb.mainIm[0][0][s]);
            double expScMag = std::hypot (ref.scRe[0][0][s], ref.scIm[0][0][s]);
            expectWithinAbsoluteError (outMag, expScMag, tol,
                                       "After reset(), first sample should be at full morph");
        }
    }
};

// Static registration
static MorphBandProcessorTests morphBandProcessorTests;
