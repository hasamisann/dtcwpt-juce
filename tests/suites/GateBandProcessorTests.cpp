#include <juce_core/juce_core.h>
#include "GateBandProcessor.h"
#include "dtcwpt/dtcwpt_band_processor.h"
#include <vector>

class GateBandProcessorTests : public juce::UnitTest
{
public:
    GateBandProcessorTests() : juce::UnitTest("GateBandProcessorTests") {}

    void runTest() override
    {
        beginTest("Gate below threshold");
        {
            GateBandProcessor gate;
            gate.prepare(44100.0, 8, 1, 1);
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

        beginTest("Gate above threshold");
        {
            GateBandProcessor gate;
            gate.prepare(44100.0, 8, 1, 1);
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
    }
};

static GateBandProcessorTests gateBandProcessorTests;
