/**
 * @file FrequencyMappingTests.cpp
 * @brief Unit tests for dtcwpt_frequency_mapping.h (path / node ID <-> frequency band).
 */

#include <dtcwpt/dtcwpt_frequency_mapping.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<std::string> allPathsOfDepth (int depth)
{
    std::vector<std::string> paths { "" };
    for (int level = 0; level < depth; ++level)
    {
        std::vector<std::string> next;
        for (const auto& p : paths)
        {
            next.push_back (p + "L");
            next.push_back (p + "H");
        }
        paths = std::move (next);
    }
    return paths;
}

int nodeIdOfPath (const std::string& path)
{
    int id = 1;
    for (const char ch : path)
        id = (ch == 'L') ? 2 * id : 2 * id + 1;
    return id;
}

} // namespace

class FrequencyMappingTests : public juce::UnitTest
{
public:
    FrequencyMappingTests() : UnitTest ("FrequencyMappingTests") {}

    void runTest() override
    {
        beginTest ("root covers the whole band");
        {
            for (const auto range : { dtcwpt::getNormalizedFrequencyRange (std::string()),
                                      dtcwpt::getNormalizedFrequencyRange (1) })
            {
                expectEquals (range.lo, 0.0);
                expectEquals (range.hi, 1.0);
            }
            expectEquals (static_cast<int> (dtcwpt::getFrequencyOrderIndex (std::string())), 0);
        }

        beginTest ("depth 3 leaves are in Gray-code order (lowest to highest)");
        {
            const std::vector<std::string> ascending { "LLL", "LLH", "LHH", "LHL", "HHL", "HHH", "HLH", "HLL" };
            for (size_t i = 0; i < ascending.size(); ++i)
            {
                const auto range = dtcwpt::getNormalizedFrequencyRange (ascending[i]);
                expectEquals (range.lo, static_cast<double> (i) / 8.0, juce::String (ascending[i]) + " lo");
                expectEquals (range.hi, static_cast<double> (i + 1) / 8.0, juce::String (ascending[i]) + " hi");
                expectEquals (static_cast<int> (dtcwpt::getFrequencyOrderIndex (ascending[i])), static_cast<int> (i),
                              juce::String (ascending[i]) + " order index");
            }
        }

        beginTest ("depth 1 and 2");
        {
            expectEquals (dtcwpt::getNormalizedFrequencyRange ("L").hi, 0.5);
            expectEquals (dtcwpt::getNormalizedFrequencyRange ("H").lo, 0.5);
            // H is mirrored: HL is the upper quarter, HH the lower of the upper half.
            expectEquals (dtcwpt::getNormalizedFrequencyRange ("HL").lo, 0.75);
            expectEquals (dtcwpt::getNormalizedFrequencyRange ("HH").hi, 0.75);
            // Under L nothing is mirrored.
            expectEquals (dtcwpt::getNormalizedFrequencyRange ("LH").lo, 0.25);
        }

        beginTest ("node ID overload agrees with the path overload; same-depth bands tile [0, 1]");
        {
            for (int depth = 0; depth <= 8; ++depth)
            {
                const auto paths = allPathsOfDepth (depth);
                std::set<std::uint64_t> indices;
                const double width = 1.0 / static_cast<double> (1 << depth);

                for (const auto& path : paths)
                {
                    const int id = nodeIdOfPath (path);
                    const auto byPath = dtcwpt::getNormalizedFrequencyRange (path);
                    const auto byId = dtcwpt::getNormalizedFrequencyRange (id);

                    expectEquals (byId.lo, byPath.lo);
                    expectEquals (byId.hi, byPath.hi);
                    expectEquals (byPath.hi - byPath.lo, width);
                    expectEquals (dtcwpt::getFrequencyOrderIndex (id), dtcwpt::getFrequencyOrderIndex (path));
                    expectEquals (byPath.lo, static_cast<double> (dtcwpt::getFrequencyOrderIndex (path)) * width);
                    indices.insert (dtcwpt::getFrequencyOrderIndex (path));
                }

                expectEquals (static_cast<int> (indices.size()), 1 << depth, "order indices are a permutation");
            }
        }

        beginTest ("a child band lies inside its parent band");
        {
            for (const auto& path : allPathsOfDepth (7))
            {
                const auto child = dtcwpt::getNormalizedFrequencyRange (path);
                const auto parent = dtcwpt::getNormalizedFrequencyRange (path.substr (0, path.size() - 1));
                expect (child.lo >= parent.lo && child.hi <= parent.hi, juce::String (path));
            }
        }

        beginTest ("the band touching Nyquist is H followed by L only");
        {
            for (int depth = 1; depth <= 12; ++depth)
            {
                const std::string top = "H" + std::string (static_cast<size_t> (depth - 1), 'L');
                expectEquals (dtcwpt::getNormalizedFrequencyRange (top).hi, 1.0, juce::String (top));
                expectEquals (dtcwpt::getNormalizedFrequencyRange (nodeIdOfPath (top)).hi, 1.0, juce::String (top));

                if (depth >= 3)
                    expect (dtcwpt::getNormalizedFrequencyRange (std::string (static_cast<size_t> (depth), 'H')).hi < 1.0,
                            "H-only path of depth " + juce::String (depth) + " is not the top band");
            }
        }

        beginTest ("sortPathsByFrequency orders mixed-depth destinations");
        {
            std::vector<std::string> paths { "HHH", "L", "HLL", "HLH", "HHL" };

            dtcwpt::sortPathsByFrequency (paths);
            expect (paths == std::vector<std::string> { "L", "HHL", "HHH", "HLH", "HLL" });

            dtcwpt::sortPathsByFrequency (paths, true);
            expect (paths == std::vector<std::string> { "HLL", "HLH", "HHH", "HHL", "L" });
        }

        beginTest ("invalid node IDs are treated as the root");
        {
            expectEquals (dtcwpt::getNormalizedFrequencyRange (0).hi, 1.0);
            expectEquals (dtcwpt::getNormalizedFrequencyRange (-5).lo, 0.0);
        }
    }
};

static FrequencyMappingTests frequencyMappingTests;
