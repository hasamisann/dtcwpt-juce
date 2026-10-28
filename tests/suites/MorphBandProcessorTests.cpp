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
 *  - Linear ramp on the host-sample time axis (shared by bands and channels)
 *  - Parameter events at stream positions, event queue overflow
 *  - Reset snaps the ramps
 */

#include "MorphBandProcessor.h"
#include "TimedLinearRamp.h"
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
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
        init (numChannels,
              std::vector<int> (static_cast<std::size_t> (numBands), numSamples),
              destIds);
    }

    // Variable sample counts per band (e.g. bands at different tree depths).
    void init (int numChannels, const std::vector<int>& samplesPerBand,
               const std::vector<int>& destIds)
    {
        destinationIds = destIds;
        const int numBands = static_cast<int> (samplesPerBand.size());

        auto alloc = [&](auto& arr)
        {
            arr.resize (static_cast<std::size_t> (numChannels));
            for (auto& ch : arr)
            {
                ch.resize (static_cast<std::size_t> (numBands));
                for (int b = 0; b < numBands; ++b)
                    ch[static_cast<std::size_t> (b)].assign (
                        static_cast<std::size_t> (samplesPerBand[static_cast<std::size_t> (b)]), 0.0);
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
                                      re[cs][bs].size() };
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

    // Fill every main sample with (mainAmp, 0) and every sidechain sample with
    // (scAmp, 0). Both have phase 0, so with phase ratio 0 the output magnitude
    // is mainAmp + m * (scAmp - mainAmp) and m can be recovered from the output.
    void fillConstant (double mainAmp, double scAmp)
    {
        for (auto* arr : { &mainRe, &scRe })
            for (auto& ch : *arr)
                for (auto& band : ch)
                    std::fill (band.begin(), band.end(), arr == &mainRe ? mainAmp : scAmp);
        for (auto* arr : { &mainIm, &scIm })
            for (auto& ch : *arr)
                for (auto& band : ch)
                    std::fill (band.begin(), band.end(), 0.0);
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

        beginTest ("7. Ramp is linear in host time — first sample not at full target");
        testSmoothedValueRamp();

        beginTest ("8. Reset snaps ramps to the requested targets");
        testReset();

        beginTest ("9. Threshold boundary preserves full phase replacement");
        testThresholdBoundaryPreservesPhaseReplacement();

        beginTest ("10. Magnitude ramp follows time, not band processing order");
        testRampIsTimeBasedAcrossBands();

        beginTest ("11. All channels use the same ramp value at the same time");
        testRampIsIdenticalAcrossChannels();

        beginTest ("12. Bypass settings do not change the ramp timing");
        testBypassDoesNotAffectRampTiming();

        beginTest ("13. 20 ms ramp completes after 20 ms of host samples");
        testRampDurationIsHostTime();

        beginTest ("14. TimedLinearRamp closed-form behaviour");
        testTimedLinearRamp();

        beginTest ("15. Parameter event starts its ramp at its stream position");
        testEventStartsRampAtStreamPosition();

        beginTest ("16. Ramp continues across internal blocks");
        testRampContinuesAcrossBlocks();

        beginTest ("17. Intermediate targets inside one internal block are kept");
        testIntermediateTargetsAreKept();

        beginTest ("18. Events beyond the block are deferred, stale events apply at block start");
        testDeferredAndStaleEvents();

        beginTest ("19. Event queue overflow keeps the latest requests");
        testEventQueueOverflow();

        beginTest ("20. Blocks without sidechain still advance the timeline");
        testNoSidechainAdvancesTimeline();

        beginTest ("21. Reset restarts the stream timeline");
        testResetRestartsTimeline();

        beginTest ("22. Bypass switches at the event time");
        testBypassSwitchesAtEventTime();
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
        proc.setTargets (0.0, 0.0, 0.001, false, false, 0);
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
        proc.setTargets (1.0, 0.0, 0.001, false, false, 0);
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
        proc2.setTargets (1.0, 0.0, 0.001, false, false, 0);
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
        proc.setTargets (0.0, 1.0, 0.001, false, false, 0);
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
        proc.setTargets (0.0, 1.0, 0.5, false, false, 0); // threshold = 0.5, sc << threshold
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
        proc.setTargets (1.0, 1.0, 0.001, /*bypassLow=*/true, /*bypassHigh=*/false, 0);
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
        proc.setTargets (1.0, 1.0, 0.001, /*bypassLow=*/false, /*bypassHigh=*/true, 0);
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
    // Test 7: Ramp
    // -----------------------------------------------------------------------
    void testSmoothedValueRamp()
    {
        // Start with magnitude 0.0 (prepared + snapped), then request 1.0 at the
        // start of the stream. One band at level 2 (4 host samples per subband
        // sample): sample s sits at tau = (s + 0.5) * 4 host samples, and the
        // linear 20 ms ramp (882 host samples at 44100 Hz) has the value tau / 882.
        constexpr int n = 128;
        SynthBandData sb;
        sb.init (1, 1, n, {5});
        sb.fillConstant (kMainAmp, kScAmp);

        MorphBandProcessor proc;
        proc.prepare (44100.0, n, 1, 1);
        proc.setTargets (1.0, 0.0, 0.001, false, false, 0);
        proc.processAllBands (sb.data);

        constexpr double rampSamples = 44100.0 * 0.02;
        constexpr double tol = 1e-9;
        for (const std::size_t s : { std::size_t { 0 }, std::size_t { 1 }, std::size_t { 64 }, std::size_t { n - 1 } })
        {
            const double tau = sampleTime (2, s);
            expectWithinAbsoluteError (usedMagnitude (sb, 0, 0, s), tau / rampSamples, tol,
                                       "Magnitude must follow the 20 ms linear ramp at the subband sample's time");
        }

        expect (usedMagnitude (sb, 0, 0, 0) < 0.01,
                "First sample should not be at the full target (ramp, not a step)");
    }

    // -----------------------------------------------------------------------
    // Test 8: Reset snaps SmoothedValues
    // -----------------------------------------------------------------------
    void testReset()
    {
        // Request a target (queued as an event), call reset(), verify that
        // subsequent processing is at target (no ramp artifact — output snaps immediately).
        SynthBandData sb;
        sb.init (1, 1, 64, {5});
        sb.fillMainSine (0.5);
        sb.fillScSine (0.5, 0.2);

        MorphBandProcessor proc;
        proc.prepare (44100.0, 64, 1, 1);
        proc.setTargets (1.0, 0.0, 0.001, false, false, 0);
        proc.reset(); // snap ramps to the requested targets and drop the pending event

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

    // -----------------------------------------------------------------------
    // Time-axis tests (issue #13)
    //
    // Band layout of a complete leaf set over one internal block of T = 8k host
    // samples (k = 8, T = 64):
    //   node 3 (H)   level 1, 4k samples
    //   node 5 (LH)  level 2, 2k samples
    //   node 8 (LLL) level 3,  k samples
    //   node 9 (LLH) level 3,  k samples
    // Subband sample s of a band at level L sits at time tau = (s + 0.5) * 2^L.
    //
    // Sample rate 1000 Hz makes the 20 ms ramp 20 host samples long, so the
    // ramp starts and ends inside the block.
    // -----------------------------------------------------------------------
    static constexpr int    kK            = 8;
    static constexpr double kRampSr       = 1000.0;
    static constexpr double kRampSamples  = 20.0;   // kRampSr * 0.02
    static constexpr double kMainAmp      = 1.0;
    static constexpr double kScAmp        = 3.0;

    static std::vector<int> timeBandIds()     { return { 3, 5, 8, 9 }; }
    static std::vector<int> timeBandLevels()  { return { 1, 2, 3, 3 }; }
    static std::vector<int> timeBandSamples() { return { 4 * kK, 2 * kK, kK, kK }; }

    // Magnitude ratio the morph used for a sample, recovered from the output.
    static double usedMagnitude (const SynthBandData& sb, int ch, int b, std::size_t s)
    {
        const auto cs = static_cast<std::size_t> (ch);
        const auto bs = static_cast<std::size_t> (b);
        const double out = std::hypot (sb.mainRe[cs][bs][s], sb.mainIm[cs][bs][s]);
        return (out - kMainAmp) / (kScAmp - kMainAmp);
    }

    static double expectedRamp (double tau) { return std::min (1.0, tau / kRampSamples); }

    static double sampleTime (int level, std::size_t s)
    {
        return (static_cast<double> (s) + 0.5) * static_cast<double> (1 << level);
    }

    // Runs one internal block with the magnitude stepping 0 -> 1.
    static void runMagnitudeStep (SynthBandData& sb, bool bypassLow, bool bypassHigh)
    {
        sb.init (2, timeBandSamples(), timeBandIds());
        sb.fillConstant (kMainAmp, kScAmp);

        MorphBandProcessor proc;
        proc.prepare (kRampSr, 4 * kK, 4, 2);
        proc.setTargets (1.0, 0.0, 0.0, bypassLow, bypassHigh, 0);
        proc.processAllBands (sb.data);
    }

    void testRampIsTimeBasedAcrossBands()
    {
        SynthBandData sb;
        runMagnitudeStep (sb, false, false);

        constexpr double tol = 1e-9;
        const auto levels  = timeBandLevels();
        const auto samples = timeBandSamples();

        int mismatches = 0;
        std::vector<std::pair<double, double>> timeline; // (tau, used magnitude), channel 0
        for (int b = 0; b < 4; ++b)
        {
            for (std::size_t s = 0; s < static_cast<std::size_t> (samples[static_cast<std::size_t> (b)]); ++s)
            {
                const double tau  = sampleTime (levels[static_cast<std::size_t> (b)], s);
                const double used = usedMagnitude (sb, 0, b, s);
                timeline.emplace_back (tau, used);
                if (std::abs (used - expectedRamp (tau)) > tol)
                    ++mismatches;
            }
        }
        expectEquals (mismatches, 0,
                      "Magnitude used for a subband sample must be the ramp value at its time tau");

        // Low bands must not be ahead of high bands: along the time axis the
        // used magnitude never decreases and never exceeds what time allows.
        std::sort (timeline.begin(), timeline.end());
        bool monotonic = true;
        for (std::size_t i = 1; i < timeline.size(); ++i)
            if (timeline[i].second < timeline[i - 1].second - tol)
                monotonic = false;
        expect (monotonic, "Used magnitude must be non-decreasing in time across all bands");
    }

    void testRampIsIdenticalAcrossChannels()
    {
        SynthBandData sb;
        runMagnitudeStep (sb, false, false);

        constexpr double tol = 1e-9;
        const auto samples = timeBandSamples();
        int mismatches = 0;
        for (int b = 0; b < 4; ++b)
            for (std::size_t s = 0; s < static_cast<std::size_t> (samples[static_cast<std::size_t> (b)]); ++s)
                if (std::abs (usedMagnitude (sb, 1, b, s) - usedMagnitude (sb, 0, b, s)) > tol)
                    ++mismatches;
        expectEquals (mismatches, 0, "Channel 1 must use the same ramp value as channel 0");

        // Channel 1 must actually ramp (it used to hold one constant value per band).
        const double first = usedMagnitude (sb, 1, 0, 0);
        const double last  = usedMagnitude (sb, 1, 0, 4 * kK - 1);
        expect (last > first + 0.1, "Channel 1 must ramp within a band");
    }

    void testBypassDoesNotAffectRampTiming()
    {
        constexpr double tol = 1e-9;
        const auto levels  = timeBandLevels();
        const auto samples = timeBandSamples();

        // Bands 1 (node 5) and 3 (node 9) are never bypassed.
        for (const bool bypass : { false, true })
        {
            SynthBandData sb;
            runMagnitudeStep (sb, /*bypassLow=*/bypass, /*bypassHigh=*/bypass);

            int mismatches = 0;
            for (const int b : { 1, 3 })
            {
                for (std::size_t s = 0; s < static_cast<std::size_t> (samples[static_cast<std::size_t> (b)]); ++s)
                {
                    const double tau = sampleTime (levels[static_cast<std::size_t> (b)], s);
                    if (std::abs (usedMagnitude (sb, 0, b, s) - expectedRamp (tau)) > tol)
                        ++mismatches;
                }
            }
            expectEquals (mismatches, 0,
                          bypass ? "Ramp timing must not change when bypassLow/bypassHigh skip bands"
                                 : "Ramp timing without bypass must follow the time axis");
        }
    }

    void testRampDurationIsHostTime()
    {
        SynthBandData sb;
        runMagnitudeStep (sb, false, false);

        constexpr double tol = 1e-9;
        const auto levels  = timeBandLevels();
        const auto samples = timeBandSamples();

        // Every sample whose time is at or after 20 ms of host samples must be
        // at the full target.
        int notFinished = 0;
        int checked = 0;
        for (int b = 0; b < 4; ++b)
        {
            for (std::size_t s = 0; s < static_cast<std::size_t> (samples[static_cast<std::size_t> (b)]); ++s)
            {
                if (sampleTime (levels[static_cast<std::size_t> (b)], s) < kRampSamples)
                    continue;
                ++checked;
                if (std::abs (usedMagnitude (sb, 0, b, s) - 1.0) > tol)
                    ++notFinished;
            }
        }
        expect (checked > 0, "Test block must extend past the end of the ramp");
        expectEquals (notFinished, 0, "Ramp must be complete after sampleRate * 0.02 host samples");
    }

    // Prepared processor with the bands of the time-axis tests, 1 channel.
    // Leaf set {2, 3}: two level-1 bands of 32 samples, T = 64.
    struct TwoBandBlock
    {
        SynthBandData sb;

        TwoBandBlock()
        {
            sb.init (1, std::vector<int> { 4 * kK, 4 * kK }, { 2, 3 });
            sb.fillConstant (kMainAmp, kScAmp);
        }

        void reset() { sb.fillConstant (kMainAmp, kScAmp); }
    };

    static constexpr std::int64_t kTwoBandLength = 8 * kK; // 64 host samples

    static void prepareTwoBand (MorphBandProcessor& proc)
    {
        proc.prepare (kRampSr, 4 * kK, 2, 1);
    }

    void testTimedLinearRamp()
    {
        constexpr double tol = 1e-12;
        TimedLinearRamp ramp;
        ramp.setCurrentAndTarget (2.0);
        expectWithinAbsoluteError (ramp.value (123.0), 2.0, tol, "Snapped ramp holds its value");
        expect (! ramp.isRamping(), "Snapped ramp is not ramping");

        ramp.retarget (4.0, 10.0);
        expectWithinAbsoluteError (ramp.value (0.0),  2.0, tol, "Ramp starts at the old value");
        expectWithinAbsoluteError (ramp.value (5.0),  3.0, tol, "Ramp is linear");
        expectWithinAbsoluteError (ramp.value (10.0), 4.0, tol, "Ramp ends at the target");
        expectWithinAbsoluteError (ramp.value (99.0), 4.0, tol, "Ramp holds the target afterwards");

        ramp.advance (5.0);
        expectWithinAbsoluteError (ramp.value (0.0), 3.0, tol, "advance() moves the origin");
        expectWithinAbsoluteError (ramp.value (2.5), 3.5, tol, "Ramp continues after advance()");

        // Retarget mid-ramp: new ramp starts from the current value with the full length.
        ramp.retarget (0.0, 10.0);
        expectWithinAbsoluteError (ramp.value (0.0), 3.0, tol, "Retarget starts from the current value");
        expectWithinAbsoluteError (ramp.value (5.0), 1.5, tol, "Retarget ramps over the full length");

        // Re-requesting the same target must not stretch the ramp.
        ramp.advance (5.0);
        ramp.retarget (0.0, 10.0);
        expectWithinAbsoluteError (ramp.value (0.0), 1.5, tol, "Same target is a no-op");
        expectWithinAbsoluteError (ramp.value (5.0), 0.0, tol, "Same target keeps the remaining time");

        ramp.advance (1000.0);
        expect (! ramp.isRamping(), "Ramp finished");

        ramp.retarget (7.0, 0.0);
        expectWithinAbsoluteError (ramp.value (0.0), 7.0, tol, "Zero-length ramp snaps");
    }

    void testEventStartsRampAtStreamPosition()
    {
        TwoBandBlock blk;
        MorphBandProcessor proc;
        prepareTwoBand (proc);
        proc.setTargets (1.0, 0.0, 0.0, false, false, 24);
        proc.processAllBands (blk.sb.data);

        constexpr double tol = 1e-9;
        int mismatches = 0;
        for (int b = 0; b < 2; ++b)
            for (std::size_t s = 0; s < 4 * kK; ++s)
            {
                const double tau = sampleTime (1, s);
                const double expected = std::clamp ((tau - 24.0) / kRampSamples, 0.0, 1.0);
                if (std::abs (usedMagnitude (blk.sb, 0, b, s) - expected) > tol)
                    ++mismatches;
            }
        expectEquals (mismatches, 0, "Ramp must start at the event's position inside the block");
    }

    void testRampContinuesAcrossBlocks()
    {
        TwoBandBlock blk;
        MorphBandProcessor proc;
        prepareTwoBand (proc);
        proc.setTargets (1.0, 0.0, 0.0, false, false, 60); // 4 host samples before the block end
        proc.processAllBands (blk.sb.data);

        constexpr double tol = 1e-9;
        // Last sample of block 0: tau = 63, 3 samples into the ramp
        expectWithinAbsoluteError (usedMagnitude (blk.sb, 0, 0, 4 * kK - 1), 3.0 / kRampSamples, tol,
                                   "Ramp is already running at the end of the first block");

        blk.reset();
        proc.processAllBands (blk.sb.data);
        // Second block: tau' = 1 is 4 + 1 = 5 samples into the ramp
        expectWithinAbsoluteError (usedMagnitude (blk.sb, 0, 0, 0), 5.0 / kRampSamples, tol,
                                   "Ramp continues into the next block");
        expectWithinAbsoluteError (usedMagnitude (blk.sb, 0, 0, 4 * kK - 1), 1.0, tol,
                                   "Ramp completes in the next block");
    }

    void testIntermediateTargetsAreKept()
    {
        // Several host blocks (8 samples each) fall into one internal block; every
        // setTargets() call is queued, none is lost.
        TwoBandBlock blk;
        MorphBandProcessor proc;
        prepareTwoBand (proc);
        proc.setTargets (1.0, 0.0, 0.0, false, false, 8);
        proc.setTargets (0.0, 0.0, 0.0, false, false, 16);
        proc.processAllBands (blk.sb.data);

        constexpr double tol = 1e-9;
        int mismatches = 0;
        for (std::size_t s = 0; s < 4 * kK; ++s)
        {
            const double tau = sampleTime (1, s);
            double expected = 0.0;
            if (tau >= 16.0)
                expected = std::max (0.0, 0.4 * (1.0 - (tau - 16.0) / kRampSamples));
            else if (tau >= 8.0)
                expected = (tau - 8.0) / kRampSamples;
            if (std::abs (usedMagnitude (blk.sb, 0, 0, s) - expected) > tol)
                ++mismatches;
        }
        expectEquals (mismatches, 0, "Ramp up to 0.4 at 16, then back down to 0");
        expect (usedMagnitude (blk.sb, 0, 0, 7) > 0.3, "The first event's ramp must be visible");
    }

    void testDeferredAndStaleEvents()
    {
        constexpr double tol = 1e-9;
        TwoBandBlock blk;
        MorphBandProcessor proc;
        prepareTwoBand (proc);

        // Event at position 74 belongs to the second block (tau' = 10)
        proc.setTargets (1.0, 0.0, 0.0, false, false, kTwoBandLength + 10);
        proc.processAllBands (blk.sb.data);
        expectWithinAbsoluteError (usedMagnitude (blk.sb, 0, 0, 4 * kK - 1), 0.0, tol,
                                   "Event beyond the block must not affect it");

        blk.reset();
        proc.processAllBands (blk.sb.data);
        expectWithinAbsoluteError (usedMagnitude (blk.sb, 0, 0, 10), (sampleTime (1, 10) - 10.0) / kRampSamples, tol,
                                   "Deferred event applies at its position in the later block");

        // A request positioned before the current block applies from the block start.
        blk.reset();
        proc.setTargets (0.0, 0.0, 0.0, false, false, 5);
        proc.processAllBands (blk.sb.data);
        // Ramp was complete at 1.0; the stale request ramps 1 -> 0 from tau = 0
        expectWithinAbsoluteError (usedMagnitude (blk.sb, 0, 0, 0), 1.0 - 1.0 / kRampSamples, tol,
                                   "Stale event applies at the block start");
    }

    void testEventQueueOverflow()
    {
        // One big block: two level-1 bands of 512 samples, T = 1024.
        constexpr int n = 512;
        SynthBandData sb;
        sb.init (1, std::vector<int> { n, n }, { 2, 3 });
        sb.fillConstant (kMainAmp, kScAmp);

        MorphBandProcessor proc;
        proc.prepare (kRampSr, n, 2, 1);

        // 100 events 8 host samples apart with increasing magnitude. The queue holds
        // 64, so the 36 oldest are dropped: the first surviving event is #37 at 296.
        for (int i = 1; i <= 100; ++i)
            proc.setTargets (static_cast<double> (i) / 100.0, 0.0, 0.0, false, false, 8 * i);
        proc.processAllBands (sb.data);

        constexpr double tol = 1e-9;
        int earlyNonZero = 0;
        for (std::size_t s = 0; s < n; ++s)
            if (sampleTime (1, s) < 296.0 && std::abs (usedMagnitude (sb, 0, 0, s)) > tol)
                ++earlyNonZero;
        expectEquals (earlyNonZero, 0, "Dropped (oldest) events must not start any ramp");

        expectWithinAbsoluteError (usedMagnitude (sb, 0, 0, n - 1), 1.0, tol,
                                   "The latest request must win");

        // First sample after the first surviving event: ramp 0 -> 0.37 started at 296
        const std::size_t s = 150; // tau = 301
        expectWithinAbsoluteError (usedMagnitude (sb, 0, 0, s), 0.37 * (301.0 - 296.0) / kRampSamples, tol,
                                   "First surviving event ramps from 0");
    }

    void testNoSidechainAdvancesTimeline()
    {
        constexpr double tol = 1e-9;
        TwoBandBlock blk;
        MorphBandProcessor proc;
        prepareTwoBand (proc);
        proc.setTargets (1.0, 0.0, 0.0, false, false, 0);

        blk.sb.data.hasSidechain = false;
        proc.processAllBands (blk.sb.data);
        expectWithinAbsoluteError (blk.sb.mainRe[0][0][3], kMainAmp, tol,
                                   "Without sidechain the samples stay untouched");

        blk.sb.data.hasSidechain = true;
        proc.processAllBands (blk.sb.data);
        expectWithinAbsoluteError (usedMagnitude (blk.sb, 0, 0, 0), 1.0, tol,
                                   "The ramp ran during the block without sidechain");
    }

    void testResetRestartsTimeline()
    {
        constexpr double tol = 1e-9;
        TwoBandBlock blk;
        MorphBandProcessor proc;
        prepareTwoBand (proc);
        proc.processAllBands (blk.sb.data);
        proc.processAllBands (blk.sb.data);   // timeline is now at 128

        proc.reset();                          // engine restarts: stream position 0 again
        blk.reset();
        proc.setTargets (1.0, 0.0, 0.0, false, false, 24);
        proc.processAllBands (blk.sb.data);
        expectWithinAbsoluteError (usedMagnitude (blk.sb, 0, 0, 20), (sampleTime (1, 20) - 24.0) / kRampSamples, tol,
                                   "After reset() positions count from the new stream start");
    }

    void testBypassSwitchesAtEventTime()
    {
        constexpr double tol = 1e-9;
        TwoBandBlock blk;
        MorphBandProcessor proc;
        prepareTwoBand (proc);
        proc.setTargets (1.0, 0.0, 0.0, false, false, 0);
        proc.processAllBands (blk.sb.data);   // settle the ramp
        blk.reset();

        // bypassLow turns on at 32: band 0 (node 2, the low band) passes through from then on
        proc.setTargets (1.0, 0.0, 0.0, true, false, kTwoBandLength + 32);
        proc.processAllBands (blk.sb.data);

        // Level-1 sample s is at tau' = 2s + 1; bypass starts at tau' = 32 -> s >= 16
        expectWithinAbsoluteError (blk.sb.mainRe[0][0][15], kScAmp, tol, "Before the event the low band is morphed");
        expectWithinAbsoluteError (blk.sb.mainRe[0][0][16], kMainAmp, tol, "From the event on the low band passes through");
        expectWithinAbsoluteError (blk.sb.mainRe[0][1][16], kScAmp, tol, "The high band stays morphed");
    }

    void testThresholdBoundaryPreservesPhaseReplacement()
    {
        SynthBandData sb;
        sb.init (1, 1, 1024, {5});

        for (std::size_t s = 0; s < 1024; ++s)
        {
            sb.mainRe[0][0][s] = 1.0;
            sb.mainIm[0][0][s] = 0.0;
            sb.scRe[0][0][s] = 0.0;
            sb.scIm[0][0][s] = 0.5;
        }

        MorphBandProcessor proc;
        proc.prepare (44100.0, 1024, 1, 1);
        proc.setTargets (0.0, 1.0, 0.5, false, false, 0);
        proc.processAllBands (sb.data);

        constexpr double tol = 1.0e-5;
        for (std::size_t s = 924; s < 1024; ++s)
        {
            const double outMag = std::hypot (sb.mainRe[0][0][s], sb.mainIm[0][0][s]);
            const double outPhase = std::atan2 (sb.mainIm[0][0][s], sb.mainRe[0][0][s]);

            expectWithinAbsoluteError (outMag, 1.0, tol,
                                       "Threshold equality should preserve the main magnitude");
            expectWithinAbsoluteError (outPhase, PI / 2.0, tol,
                                       "Threshold equality should not attenuate sidechain phase replacement");
        }
    }
};

// Static registration
static MorphBandProcessorTests morphBandProcessorTests;
