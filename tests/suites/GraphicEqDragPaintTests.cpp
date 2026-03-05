#include "../../sample_plugins/common/GraphicEqDragPaint.h"

#include <juce_core/juce_core.h>

#include <vector>

class GraphicEqDragPaintTests : public juce::UnitTest
{
public:
    GraphicEqDragPaintTests() : juce::UnitTest ("GraphicEqDragPaintTests") {}

    void runTest() override
    {
        beginTest ("enumerateCrossedBands ascending");
        {
            const auto bands = graphic_eq::enumerateCrossedBands (3, 7, 16);
            expectEquals (static_cast<int> (bands.size()), 5);
            expectEquals (bands[0], 3);
            expectEquals (bands[1], 4);
            expectEquals (bands[2], 5);
            expectEquals (bands[3], 6);
            expectEquals (bands[4], 7);
        }

        beginTest ("enumerateCrossedBands descending");
        {
            const auto bands = graphic_eq::enumerateCrossedBands (7, 3, 16);
            expectEquals (static_cast<int> (bands.size()), 5);
            expectEquals (bands[0], 7);
            expectEquals (bands[1], 6);
            expectEquals (bands[2], 5);
            expectEquals (bands[3], 4);
            expectEquals (bands[4], 3);
        }

        beginTest ("enumerateCrossedBands rejects invalid indices");
        {
            expect (graphic_eq::enumerateCrossedBands (-1, 3, 16).empty());
            expect (graphic_eq::enumerateCrossedBands (2, 16, 16).empty());
            expect (graphic_eq::enumerateCrossedBands (2, 3, 0).empty());
        }

        beginTest ("interpolateYForX computes expected midpoint");
        {
            const auto y = graphic_eq::interpolateYForX (100.0f, 20.0f, 220.0f, 80.0f, 160.0f);
            expectWithinAbsoluteError (y, 50.0f, 1.0e-4f);
        }

        beginTest ("dense crossing includes all intermediate bands exactly once");
        {
            const auto bands = graphic_eq::enumerateCrossedBands (12, 20, 64);
            expectEquals (static_cast<int> (bands.size()), 9);
            expectEquals (bands.front(), 12);
            expectEquals (bands.back(), 20);

            for (int i = 0; i < static_cast<int> (bands.size()); ++i)
                expectEquals (bands[static_cast<std::size_t> (i)], 12 + i);
        }
    }
};

static GraphicEqDragPaintTests graphicEqDragPaintTests;
