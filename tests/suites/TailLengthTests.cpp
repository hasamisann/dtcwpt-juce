/**
 * @file TailLengthTests.cpp
 * @brief Measures the filter-bank impulse response that continues past the
 *        reported latency and compares it with DTCWPTProcessor::getTailLengthSamples().
 */

#include <dtcwpt/dtcwpt_processor.h>
#include <dtcwpt/dtcwpt_band_processor.h>
#include "../util/TestUtils.h"

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

class TailLengthTests : public juce::UnitTest
{
public:
    TailLengthTests() : UnitTest ("TailLengthTests") {}

    void runTest() override
    {
        beginTest ("Unmodified subbands give a pure delay at the reported latency");
        testPassthroughIsPureDelay();

        beginTest ("Zeroing one subband leaves a measurable response after the latency");
        testZeroedSubbandResponseOutlastsLatency();

        beginTest ("getTailLengthSamples bounds the measured tail and is tight to the exact support");
        testTailLengthBoundsAndMatchesExactSupport();

        beginTest ("getTailLengthSamples is zero before prepareToPlay");
        testTailLengthIsZeroBeforePrepare();
    }

private:
    static constexpr int kBlockSize = 512;
    static constexpr double kSignificanceThreshold = 1.0e-6;

    struct Topology
    {
        juce::String name;
        std::vector<std::string> destinations;
        int depth = 0;
        std::string adjacentLowBand;  // deepest low-side neighbour of the lowest band ("LH" at the deepest level)
    };

    /** Octave-split DWT: H, LH, LLH, ... plus the residual L...L at the deepest level. */
    static Topology makeDwt (int levels)
    {
        Topology t;
        t.name = juce::String (levels) + "-level DWT";
        t.depth = levels;
        for (int k = 0; k < levels; ++k)
            t.destinations.push_back (std::string (static_cast<size_t> (k), 'L') + "H");
        t.destinations.push_back (std::string (static_cast<size_t> (levels), 'L'));
        t.adjacentLowBand = std::string (static_cast<size_t> (levels - 1), 'L') + "H";
        return t;
    }

    static Topology makeFullPacket (int depth)
    {
        Topology t;
        t.name = "depth-" + juce::String (depth) + " full packet";
        t.depth = depth;
        t.destinations = TestUtils::getFullPacketDestinations (depth);
        t.adjacentLowBand = std::string (static_cast<size_t> (depth - 1), 'L') + "H";
        return t;
    }

    static dtcwpt::TopologyConfig makeConfig (const Topology& topology)
    {
        dtcwpt::TopologyConfig config;
        config.destinations = topology.destinations;
        config.maxDepth = std::max (topology.depth, 1);
        return config;
    }

    static int indexOf (const Topology& topology, const std::string& destination)
    {
        const auto it = std::find (topology.destinations.begin(), topology.destinations.end(), destination);
        return it == topology.destinations.end() ? -1 : static_cast<int> (it - topology.destinations.begin());
    }

    enum class BandMode
    {
        passthrough,
        zeroOnly,  ///< zero the selected band, keep the others
        keepOnly   ///< keep only the selected band
    };

    /** Impulse placed at input sample @p phase. Returns output samples (index n = input index n). */
    static std::vector<double> impulseResponse (const Topology& topology,
                                                int phase,
                                                BandMode mode,
                                                int bandIndex,
                                                int& latencyOut)
    {
        dtcwpt::DTCWPTProcessor processor;
        processor.setBandProcessor (dtcwpt::makeLambdaProcessor (
            [mode, bandIndex] (int band, double* re, double* im, size_t numSamples)
            {
                const bool zero = (mode == BandMode::zeroOnly && band == bandIndex)
                               || (mode == BandMode::keepOnly && band != bandIndex);
                if (zero)
                {
                    std::fill (re, re + numSamples, 0.0);
                    std::fill (im, im + numSamples, 0.0);
                }
            }));
        processor.prepareToPlay (48000.0, kBlockSize, makeConfig (topology), 1);

        const int latency = processor.getLatency();
        latencyOut = latency;

        // The filter bank response cannot exceed a few filter lengths at the coarsest rate.
        const int horizon = 4 * (14 << topology.depth) + (1 << topology.depth);
        const int total = phase + latency + horizon;

        std::vector<double> output;
        output.reserve (static_cast<size_t> (total + kBlockSize));

        int cursor = 0;
        while (cursor < total)
        {
            const int n = std::min (kBlockSize, total - cursor);
            juce::AudioBuffer<double> buffer (1, n);
            buffer.clear();
            if (phase >= cursor && phase < cursor + n)
                buffer.setSample (0, phase - cursor, 1.0);

            processor.processBlock (buffer);
            const auto* data = buffer.getReadPointer (0);
            output.insert (output.end(), data, data + n);
            cursor += n;
        }
        return output;
    }

    /** Index of the last sample whose magnitude exceeds @p threshold, or -1. */
    static int lastAbove (const std::vector<double>& samples, double threshold)
    {
        for (int i = static_cast<int> (samples.size()) - 1; i >= 0; --i)
            if (std::abs (samples[static_cast<size_t> (i)]) > threshold)
                return i;
        return -1;
    }

    /** Impulse phases relative to the 2^depth decimation grid: all of them up to depth 8, 16 evenly spaced beyond. */
    static std::vector<int> phasesFor (int depth)
    {
        const int period = 1 << depth;
        const int step = depth <= 8 ? 1 : period / 16;
        std::vector<int> phases;
        for (int p = 0; p < period; p += step)
            phases.push_back (p);
        return phases;
    }

    /** Largest (last significant sample - latency) over the swept phases; -1 if the output never exceeds the threshold. */
    int measureTail (const Topology& topology, BandMode mode, const std::string& band, double threshold)
    {
        const int bandIndex = indexOf (topology, band);
        expect (bandIndex >= 0 || mode == BandMode::passthrough, juce::String ("Band must exist in the topology: ") + band);

        int tail = -1;
        for (const int phase : phasesFor (topology.depth))
        {
            int latency = 0;
            const auto out = impulseResponse (topology, phase, mode, bandIndex, latency);
            const int last = lastAbove (out, threshold);
            tail = std::max (tail, last - (phase + latency));
        }
        return tail;
    }

    static std::vector<Topology> topologies()
    {
        return { makeDwt (6), makeDwt (8), makeDwt (12), makeFullPacket (3) };
    }

    void testPassthroughIsPureDelay()
    {
        const auto topology = makeDwt (6);
        constexpr int phase = 5;
        int latency = 0;
        const auto out = impulseResponse (topology, phase, BandMode::passthrough, -1, latency);

        int significant = 0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            if (std::abs (out[i]) > kSignificanceThreshold)
            {
                ++significant;
                expectEquals (static_cast<int> (i), phase + latency,
                              "Unmodified impulse must appear only at the reported latency");
            }
        }
        expectEquals (significant, 1, "Unmodified impulse must be a single sample");
    }

    void testZeroedSubbandResponseOutlastsLatency()
    {
        for (const auto& topology : topologies())
        {
            const int measured = measureTail (topology, BandMode::zeroOnly,
                                              topology.adjacentLowBand, kSignificanceThreshold);
            logMessage (topology.name + ": zeroing " + juce::String (topology.adjacentLowBand)
                        + " leaves |h| > 1e-6 for " + juce::String (measured)
                        + " samples after the reported latency");
            expect (measured > 0,
                    topology.name + ": response must continue after the reported latency, measured "
                        + juce::String (measured));
        }
    }

    void testTailLengthBoundsAndMatchesExactSupport()
    {
        for (const auto& topology : topologies())
        {
            dtcwpt::DTCWPTProcessor processor;
            processor.prepareToPlay (48000.0, kBlockSize, makeConfig (topology), 1);
            const int api = processor.getTailLengthSamples();

            // (1) Upper bound of the response the issue measures (zero the neighbouring band, |h| > 1e-6).
            const int measured = measureTail (topology, BandMode::zeroOnly,
                                              topology.adjacentLowBand, kSignificanceThreshold);
            expect (api >= measured,
                    topology.name + ": tail " + juce::String (api)
                        + " must cover the measured " + juce::String (measured));

            // (2) Tight against the exact FIR support: keep one band at a time and look for any non-zero
            //     sample. Only the deepest bands can set the maximum, so shallower ones are skipped for 12 levels.
            int exact = 0;
            for (const auto& band : topology.destinations)
            {
                if (topology.depth >= 12 && static_cast<int> (band.size()) < topology.depth)
                    continue;
                exact = std::max (exact, measureTail (topology, BandMode::keepOnly, band, 0.0));
            }
            logMessage (topology.name + ": getTailLengthSamples = " + juce::String (api)
                        + ", exact support = " + juce::String (exact)
                        + ", |h| > 1e-6 measurement = " + juce::String (measured));
            expect (api >= exact,
                    topology.name + ": tail " + juce::String (api)
                        + " must cover the exact support " + juce::String (exact));
            expect (api - exact <= (1 << topology.depth),
                    topology.name + ": tail " + juce::String (api)
                        + " must not exceed the exact support " + juce::String (exact)
                        + " by more than 2^depth");
        }
    }

    void testTailLengthIsZeroBeforePrepare()
    {
        dtcwpt::DTCWPTProcessor processor;
        expectEquals (processor.getTailLengthSamples(), 0, "Unprepared engine has no tail");
    }
};

static TailLengthTests tailLengthTests;
