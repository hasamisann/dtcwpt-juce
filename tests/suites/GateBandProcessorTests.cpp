#include <juce_core/juce_core.h>
#include "GateBandProcessor.h"
#include "dtcwpt/dtcwpt_band_processor.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

class GateBandProcessorTests : public juce::UnitTest
{
public:
    GateBandProcessorTests() : juce::UnitTest("GateBandProcessorTests") {}

    void runTest() override
    {
        beginTest("Enabling stereo link converges to one shared gain in 20 ms");
        testStereoLinkTransition();

        beginTest("Gate below threshold (zero time constants: instantaneous gate)");
        {
            GateBandProcessor gate;
            gate.prepare(44100.0, 8, 1, 1);
            gate.setDynamics(instantDynamics());
            double thresholds[1] = { 0.01 };
            gate.setThresholds(thresholds, 1);

            dtcwpt::BandData data;
            data.numChannels = 1;
            data.numBands = 1;
            double re[8] = { 0.005, 0.005, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
            double im[8] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
            dtcwpt::ChannelBandView view { re, im, 8 };
            data.bands = { { view } };

            gate.processAllBands(data);
            
            for (int i = 0; i < 8; ++i)
            {
                expectEquals(re[i], 0.0);
                expectEquals(im[i], 0.0);
            }
        }

        beginTest("Gate above threshold (zero time constants: instantaneous gate)");
        {
            GateBandProcessor gate;
            gate.prepare(44100.0, 8, 1, 1);
            gate.setDynamics(instantDynamics());
            double thresholds[1] = { 0.01 };
            gate.setThresholds(thresholds, 1);

            dtcwpt::BandData data;
            data.numChannels = 1;
            data.numBands = 1;
            double re[8] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };
            double im[8] = { 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3 };
            dtcwpt::ChannelBandView view { re, im, 8 };
            data.bands = { { view } };

            gate.processAllBands(data);
            
            for (int i = 0; i < 8; ++i)
            {
                expectEquals(re[i], 0.5);
                expectEquals(im[i], 0.3);
            }
        }

        beginTest("Gate at boundary");
        {
            GateBandProcessor gate;
            gate.prepare(44100.0, 2, 1, 1);
            gate.setDynamics(instantDynamics());
            double thresholds[1] = { 0.01 };
            gate.setThresholds(thresholds, 1);

            dtcwpt::BandData data;
            data.numChannels = 1;
            data.numBands = 1;
            double re[2] = { 0.01, 0.0 };
            double im[2] = { 0.0, 0.0 };
            dtcwpt::ChannelBandView view { re, im, 1 };
            data.bands = { { view } };

            gate.processAllBands(data);
            
            expectEquals(re[0], 0.01);
            expectEquals(im[0], 0.0);
        }

        beginTest("Band level computation");
        {
            GateBandProcessor gate;
            gate.prepare(44100.0, 2, 1, 1);
            double thresholds[1] = { 1e-6 }; // fully open
            gate.setThresholds(thresholds, 1);

            dtcwpt::BandData data;
            data.numChannels = 1;
            data.numBands = 1;
            double re[2] = { 0.1, 0.1 };
            double im[2] = { 0.0, 0.0 };
            dtcwpt::ChannelBandView view { re, im, 2 };
            data.bands = { { view } };

            gate.processAllBands(data);
            
            float levels[1] = { 0.0f };
            gate.getBandLevelsDb(levels);
            
            expectWithinAbsoluteError(levels[0], -20.0f, 1.0f);
        }

        beginTest("Multi-band and Reset");
        {
            GateBandProcessor gate;
            gate.prepare(44100.0, 8, 2, 1);
            gate.setDynamics(instantDynamics());
            double thresholds[2] = { 0.01, 0.9 };
            gate.setThresholds(thresholds, 2);

            dtcwpt::BandData data;
            data.numChannels = 1;
            data.numBands = 2;
            double re0[1] = { 0.5 };
            double im0[1] = { 0.0 };
            double re1[1] = { 0.5 };
            double im1[1] = { 0.0 };
            
            dtcwpt::ChannelBandView view0 { re0, im0, 1 };
            dtcwpt::ChannelBandView view1 { re1, im1, 1 };
            data.bands = { { view0, view1 } };

            gate.processAllBands(data);
            
            // Band 0: above threshold (0.5 > 0.01) -> unchanged
            expectEquals(re0[0], 0.5);
            // Band 1: below threshold (0.5 < 0.9) -> zeroed
            expectEquals(re1[0], 0.0);
            
            // Reset clears levels
            gate.reset();
            float levels[2] = { 0.0f, 0.0f };
            gate.getBandLevelsDb(levels);
            expectEquals(levels[0], -80.0f);
            expectEquals(levels[1], -80.0f);
        }

        beginTest("Lowest band bypass preserves samples while higher bands still gate");
        {
            GateBandProcessor gate;
            gate.prepare(44100.0, 4, 2, 1);
            gate.setBypassLowest(true);

            const double thresholds[2] = { 0.9, 0.9 };
            gate.setThresholds(thresholds, 2);

            std::vector<int> destinationIds{4, 5};
            double lowRe[2] = { 0.25, 0.25 };
            double lowIm[2] = { 0.0, 0.0 };
            double highRe[2] = { 0.25, 0.25 };
            double highIm[2] = { 0.0, 0.0 };

            dtcwpt::BandData data;
            data.numChannels = 1;
            data.numBands = 2;
            data.destinationIds = &destinationIds;
            dtcwpt::ChannelBandView lowView { lowRe, lowIm, 2 };
            dtcwpt::ChannelBandView highView { highRe, highIm, 2 };
            data.bands = { { lowView, highView } };

            gate.processAllBands(data);

            expectEquals(lowRe[0], 0.25);
            expectEquals(lowRe[1], 0.25);
            expectEquals(highRe[0], 0.0);
            expectEquals(highRe[1], 0.0);
        }

        beginTest("Threshold equality leaves a band open deterministically (zero time constants)");
        {
            GateBandProcessor gate;
            gate.prepare(44100.0, 2, 1, 1);
            gate.setDynamics(instantDynamics());
            const double threshold[1] = { 0.25 };
            gate.setThresholds(threshold, 1);

            double re[2] = { 0.25, -0.25 };
            double im[2] = { 0.0, 0.0 };
            dtcwpt::BandData data;
            data.numChannels = 1;
            data.numBands = 1;
            dtcwpt::ChannelBandView view { re, im, 2 };
            data.bands = { { view } };

            gate.processAllBands(data);

            expectEquals(re[0], 0.25);
            expectEquals(re[1], -0.25);
        }

        beginTest("Default dynamics: signals below the threshold are attenuated, above it pass");
        {
            const GateBandProcessor::DynamicsParams defaults;
            const std::vector<double> quiet(20000, 0.005);
            const std::vector<double> loud(20000, 0.5);

            const auto below = runGate(defaults, { 0.01 }, { quiet }, 512);
            const auto above = runGate(defaults, { 0.01 }, { loud }, 512);
            expectWithinAbsoluteError(gainOf(below, 0, { quiet }).back(), 0.0, 1.0e-9, "below threshold: attenuated to the range gain");
            expectWithinAbsoluteError(gainOf(above, 0, { loud }).back(), 1.0, 1.0e-6, "above threshold: passes after the attack");
        }

        beginTest("Lowest band bypass is untouched by the dynamics while other bands gate");
        {
            GateBandProcessor gate;
            gate.prepare(44100.0, 512, 2, 2);
            gate.setBypassLowest(true);
            const double thresholds[2] = { 0.9, 0.9 };
            gate.setThresholds(thresholds, 2);

            const std::vector<int> ids { 4, 5 };
            std::vector<std::vector<double>> lowRe(2, std::vector<double>(512, 0.25));
            std::vector<std::vector<double>> highRe(2, std::vector<double>(512, 0.25));
            std::vector<std::vector<double>> im(2, std::vector<double>(512, 0.0));
            dtcwpt::BandData data;
            data.numChannels = 2;
            data.numBands = 2;
            data.hasSidechain = false;
            data.destinationIds = &ids;
            data.bands.resize(2);
            for (size_t ch = 0; ch < 2; ++ch)
            {
                data.bands[ch].push_back({ lowRe[ch].data(), im[ch].data(), 512 });
                data.bands[ch].push_back({ highRe[ch].data(), im[ch].data(), 512 });
            }

            gate.processAllBands(data);

            for (size_t n = 0; n < 512; ++n)
            {
                expectEquals(lowRe[0][n], 0.25);
                expectEquals(lowRe[1][n], 0.25);
                expectEquals(highRe[0][n], 0.0);
                expectEquals(highRe[1][n], 0.0);
            }
        }

        beginTest("Gain changes between adjacent samples are bounded by the attack/release coefficients");
        {
            GateBandProcessor::DynamicsParams params;
            params.attackMs = 5.0;
            params.releaseMs = 20.0;
            params.holdMs = 0.0;
            params.hysteresisDb = 0.0;
            params.stereoLink = false;

            // |c| oscillates 0.05..0.15 around the 0.1 threshold, so the gate must open and close.
            const size_t total = 20000;
            std::vector<double> mag(total);
            for (size_t n = 0; n < total; ++n)
                mag[n] = 0.1 * (1.0 + 0.5 * std::sin(2.0 * kPi * static_cast<double>(n) / 4000.0));

            const auto result = runGate(params, { 0.1 }, { mag }, 512);
            const auto gain = gainOf(result, 0, { mag });

            double maxStep = 0.0;
            for (size_t n = 1; n < gain.size(); ++n)
                maxStep = std::max(maxStep, std::abs(gain[n] - gain[n - 1]));

            const double bound = std::max(coefficient(params.attackMs, kFs), coefficient(params.releaseMs, kFs));
            expect(maxStep <= bound + 1.0e-9,
                   "max adjacent gain step " + juce::String(maxStep) + " must not exceed " + juce::String(bound));
            expect(*std::min_element(gain.begin(), gain.end()) < 0.1 && *std::max_element(gain.begin(), gain.end()) > 0.9,
                   "the gate should still open and close fully for the modulated signal");
        }

        beginTest("Hysteresis reduces open/close chatter near the threshold");
        {
            const size_t total = 64 * 40;
            std::vector<double> mag(total);
            // |c| swings 0.8..1.2 x threshold with a period of 64 subband samples.
            for (size_t n = 0; n < total; ++n)
                mag[n] = 0.1 * (1.0 + 0.2 * std::sin(2.0 * kPi * static_cast<double>(n) / 64.0));

            GateBandProcessor::DynamicsParams params;
            params.attackMs = 0.05;
            params.releaseMs = 0.05;
            params.holdMs = 0.0;
            params.stereoLink = false;

            params.hysteresisDb = 0.0;
            const auto noHyst = gainOf(runGate(params, { 0.1 }, { mag }, 256), 0, { mag });
            params.hysteresisDb = 6.0;
            const auto withHyst = gainOf(runGate(params, { 0.1 }, { mag }, 256), 0, { mag });

            const int chatterNone = countCrossings(noHyst, 0.5);
            const int chatterHyst = countCrossings(withHyst, 0.5);
            expect(chatterNone >= 10, "without hysteresis the gate should chatter (" + juce::String(chatterNone) + ")");
            expect(chatterHyst < chatterNone,
                   "6 dB hysteresis should reduce transitions: " + juce::String(chatterHyst) + " vs " + juce::String(chatterNone));
        }

        beginTest("Hold keeps the gate open for the configured time after the level drops");
        {
            GateBandProcessor::DynamicsParams params;
            params.attackMs = 0.05;
            params.releaseMs = 0.05;
            params.holdMs = 10.0; // 441 samples at fs
            params.hysteresisDb = 0.0;
            params.stereoLink = false;

            std::vector<double> mag(4000, 0.5);
            std::fill(mag.begin() + 1000, mag.end(), 0.0001);

            const auto gain = gainOf(runGate(params, { 0.1 }, { mag }, 512), 0, { mag });
            // The input is quiet from sample 1000; the gate may only start closing after the hold time (441 samples).
            expect(gain[1000 + 400] > 0.9, "gate should still be open inside the hold time");
            expect(gain[1000 + 441 + 100] < 0.1, "gate should have closed shortly after the hold time");
        }

        beginTest("Stereo link applies the same gain to both channels");
        {
            GateBandProcessor::DynamicsParams params;
            params.attackMs = 1.0;
            params.releaseMs = 10.0;
            params.holdMs = 0.0;
            params.rangeDb = -std::numeric_limits<double>::infinity();

            const size_t total = 8000;
            const std::vector<double> loud(total, 0.5);
            const std::vector<double> quiet(total, 0.01); // below the 0.1 threshold on its own

            params.stereoLink = true;
            const auto linked = runGate(params, { 0.1 }, { loud, quiet }, 512);
            const auto gl = gainOf(linked, 0, { loud, quiet });
            const auto gr = gainOf(linked, 1, { loud, quiet });
            double maxDiff = 0.0;
            for (size_t n = 0; n < total; ++n)
                maxDiff = std::max(maxDiff, std::abs(gl[n] - gr[n]));
            expect(maxDiff < 1.0e-12, "linked channels should share one gain (max diff " + juce::String(maxDiff) + ")");
            expect(gr.back() > 0.99, "the quiet channel follows the louder channel when linked");

            params.stereoLink = false;
            const auto unlinked = runGate(params, { 0.1 }, { loud, quiet }, 512);
            const auto ur = gainOf(unlinked, 1, { loud, quiet });
            expect(gainOf(unlinked, 0, { loud, quiet }).back() > 0.99, "unlinked loud channel stays open");
            expect(ur.back() < 1.0e-3, "unlinked quiet channel is gated independently");
        }

        beginTest("Range sets the closed-state gain");
        {
            GateBandProcessor::DynamicsParams params;
            params.attackMs = 1.0;
            params.releaseMs = 5.0;
            params.holdMs = 0.0;
            params.stereoLink = false;

            const std::vector<double> quiet(20000, 0.01);

            params.rangeDb = -20.0;
            const auto ranged = gainOf(runGate(params, { 0.1 }, { quiet }, 512), 0, { quiet });
            expectWithinAbsoluteError(ranged.back(), 0.1, 1.0e-6, "-20 dB range should leave 0.1 gain when closed");

            params.rangeDb = -std::numeric_limits<double>::infinity();
            const auto muted = gainOf(runGate(params, { 0.1 }, { quiet }, 512), 0, { quiet });
            expectWithinAbsoluteError(muted.back(), 0.0, 1.0e-9, "-inf range should fully mute when closed");
        }

        beginTest("Threshold changes are smoothed across block boundaries");
        {
            GateBandProcessor::DynamicsParams params;
            params.attackMs = 0.05;
            params.releaseMs = 0.05;
            params.holdMs = 0.0;
            params.hysteresisDb = 0.0;
            params.stereoLink = false;

            GateBandProcessor gate;
            gate.prepare(kFs, 256, 1, 1);
            gate.setDynamics(params);

            const std::vector<double> mag(256, 0.5);
            auto processBlock = [&](double threshold)
            {
                gate.setThresholds(&threshold, 1);
                std::vector<double> re = mag, im(mag.size(), 0.0);
                dtcwpt::ChannelBandView view { re.data(), im.data(), re.size() };
                dtcwpt::BandData data;
                data.numChannels = 1;
                data.numBands = 1;
                data.hasSidechain = false;
                data.destinationIds = nullptr;
                data.bands = { { view } };
                gate.processAllBands(data);
                return re;
            };

            for (int block = 0; block < 4; ++block)
                processBlock(0.1);
            const auto settled = processBlock(0.1);
            expectWithinAbsoluteError(settled.back() / 0.5, 1.0, 1.0e-6, "gate is open before the threshold step");

            // Step the threshold from 0.1 to 0.9 (above |c| = 0.5). A hard switch would close at the first sample.
            const auto stepped = processBlock(0.9);
            double minGain = 1.0;
            for (size_t n = 0; n < 128; ++n)
                minGain = std::min(minGain, stepped[n] / 0.5);
            expect(minGain > 0.99,
                   "the first 128 samples after the step must still be open (min gain " + juce::String(minGain) + ")");

            std::vector<double> last;
            for (int block = 0; block < 40; ++block)
                last = processBlock(0.9);
            expect(last.back() / 0.5 < 1.0e-3, "the gate closes once the smoothed threshold has passed the signal level");
        }

        beginTest("Time constants follow the subband sample rate of the node depth");
        {
            GateBandProcessor::DynamicsParams params;
            params.attackMs = 8.0;
            params.releaseMs = 8.0;
            params.holdMs = 0.0;
            params.hysteresisDb = 0.0;
            params.stereoLink = false;

            const std::vector<double> mag(3000, 0.5);
            auto timeToTarget = [&](int nodeId)
            {
                const std::vector<int> ids { nodeId };
                const auto result = runGate(params, { 1.0e-9 }, { mag }, 512, &ids);
                const auto gain = gainOf(result, 0, { mag });
                for (size_t n = 0; n < gain.size(); ++n)
                    if (gain[n] >= 1.0 - std::exp(-1.0))
                        return static_cast<int>(n);
                return -1;
            };

            const int level0 = timeToTarget(1);  // fs_b = fs
            const int level3 = timeToTarget(9);  // fs_b = fs / 8
            expect(level0 > 0 && level3 > 0, "gain should reach 1-1/e at both depths");
            expectWithinAbsoluteError(static_cast<double>(level0), 8.0e-3 * kFs, 3.0, "level 0 reaches 1-1/e after one time constant");
            expectWithinAbsoluteError(static_cast<double>(level3), 8.0e-3 * kFs / 8.0, 3.0, "level 3 uses fs/8 for the same time constant");
        }

        beginTest("Reset returns the gate to its closed initial state");
        {
            GateBandProcessor::DynamicsParams params;
            params.attackMs = 5.0;
            params.releaseMs = 5.0;
            params.holdMs = 0.0;
            params.stereoLink = false;

            const std::vector<double> loud(4000, 0.5);
            GateBandProcessor gate;
            gate.prepare(kFs, 512, 1, 1);
            gate.setDynamics(params);
            const double threshold = 0.1;
            gate.setThresholds(&threshold, 1);

            auto run = [&]()
            {
                std::vector<double> re = loud, im(loud.size(), 0.0);
                dtcwpt::ChannelBandView view { re.data(), im.data(), re.size() };
                dtcwpt::BandData data;
                data.numChannels = 1;
                data.numBands = 1;
                data.hasSidechain = false;
                data.destinationIds = nullptr;
                data.bands = { { view } };
                gate.processAllBands(data);
                return re;
            };

            const auto first = run();
            const auto warm = run();
            expect(warm[0] / 0.5 > 0.99, "an open gate carries its state into the next block");

            gate.reset();
            const auto afterReset = run();
            expect(afterReset[0] / 0.5 < 0.1, "after reset the gate starts closed and opens with the attack time");
            expectWithinAbsoluteError(afterReset[0], first[0], 1.0e-12, "post-reset output reproduces a fresh processor");
        }
    }

private:
    void testStereoLinkTransition()
    {
        GateBandProcessor gate;
        gate.prepare(48000.0, 24000, 1, 2);
        double threshold = 0.1;
        gate.setThresholds(&threshold, 1);
        GateBandProcessor::DynamicsParams params;
        params.attackMs = 100.0;
        params.holdMs = 0.0;
        params.stereoLink = false;
        gate.setDynamics(params);
        std::vector<int> ids { 2 };
        std::vector<double> left(24000, 0.5), right(24000, 0.01), imag(24000, 0.0);
        dtcwpt::BandData data;
        data.numChannels = 2;
        data.numBands = 1;
        data.destinationIds = &ids;
        data.bands = {{{left.data(), imag.data(), left.size()}},
                      {{right.data(), imag.data(), right.size()}}};
        gate.processAllBands(data);
        const double oldLeftGain = left.back() / 0.5;
        const double oldRightGain = right.back() / 0.01;
        params.stereoLink = true;
        gate.setDynamics(params);
        std::fill(left.begin(), left.end(), 0.5);
        std::fill(right.begin(), right.end(), 0.01);
        data.bands[0][0].numSamples = data.bands[1][0].numSamples = 480;
        gate.processAllBands(data); // depth 1: 480 coefficients = 20 ms
        expectWithinAbsoluteError(left[479] / 0.5, right[479] / 0.01, 1.0e-12);
        // Switching modes must not jump either gain to the other's old state.
        expect(std::abs(left[0] / 0.5 - oldLeftGain) < 0.01);
        expect(std::abs(right[0] / 0.01 - oldRightGain) < 0.01);
    }

    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kFs = 44100.0;

    struct Streams
    {
        std::vector<std::vector<double>> re, im;
    };

    /** Zero time constants, no hold/hysteresis, -inf range, unlinked: degenerates to the former hard gate. */
    static GateBandProcessor::DynamicsParams instantDynamics()
    {
        GateBandProcessor::DynamicsParams params;
        params.attackMs = 0.0;
        params.holdMs = 0.0;
        params.releaseMs = 0.0;
        params.hysteresisDb = 0.0;
        params.stereoLink = false;
        return params;
    }

    static double coefficient(double timeMs, double fs)
    {
        return 1.0 - std::exp(-1.0 / (timeMs * 1.0e-3 * fs));
    }

    /** Runs one band (rotating phasor with the given |c| per channel) block by block through a fresh gate. */
    static Streams runGate(const GateBandProcessor::DynamicsParams& params,
                           const std::vector<double>& thresholds,
                           const std::vector<std::vector<double>>& magnitudes,
                           size_t blockSize,
                           const std::vector<int>* destinationIds = nullptr)
    {
        const size_t numChannels = magnitudes.size();
        const size_t total = magnitudes.front().size();

        GateBandProcessor gate;
        gate.prepare(kFs, static_cast<int>(blockSize), 1, static_cast<int>(numChannels));
        gate.setDynamics(params);
        gate.setThresholds(thresholds.data(), 1);

        Streams out;
        out.re.assign(numChannels, std::vector<double>(total));
        out.im.assign(numChannels, std::vector<double>(total));
        for (size_t ch = 0; ch < numChannels; ++ch)
            for (size_t n = 0; n < total; ++n)
            {
                const double phase = 2.0 * kPi * 0.05 * static_cast<double>(n);
                out.re[ch][n] = magnitudes[ch][n] * std::cos(phase);
                out.im[ch][n] = magnitudes[ch][n] * std::sin(phase);
            }

        for (size_t start = 0; start < total; start += blockSize)
        {
            const size_t count = std::min(blockSize, total - start);
            dtcwpt::BandData data;
            data.numChannels = static_cast<int>(numChannels);
            data.numBands = 1;
            data.hasSidechain = false;
            data.destinationIds = destinationIds;
            data.bands.resize(numChannels);
            for (size_t ch = 0; ch < numChannels; ++ch)
                data.bands[ch].push_back({ out.re[ch].data() + start, out.im[ch].data() + start, count });
            gate.processAllBands(data);
        }
        return out;
    }

    /** Output |c| / input |c| per sample. */
    static std::vector<double> gainOf(const Streams& out, size_t ch, const std::vector<std::vector<double>>& magnitudes)
    {
        std::vector<double> gain(magnitudes[ch].size());
        for (size_t n = 0; n < gain.size(); ++n)
            gain[n] = std::hypot(out.re[ch][n], out.im[ch][n]) / magnitudes[ch][n];
        return gain;
    }

    static int countCrossings(const std::vector<double>& gain, double level)
    {
        int count = 0;
        for (size_t n = 1; n < gain.size(); ++n)
            if ((gain[n - 1] < level) != (gain[n] < level))
                ++count;
        return count;
    }
};

static GateBandProcessorTests gateBandProcessorTests;
