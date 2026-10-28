/**
 * @file TopologyStateTests.cpp
 * @brief JUCE UnitTest suite for TopologyState.
 *
 * Tests cover:
 *  1. Initial state: dirty is false, tryConsume returns false.
 *  2. requestChange sets dirty; tryConsume returns true with expected data.
 *  3. tryConsume clears dirty; second call returns false.
 *  4. Multiple requestChange calls: tryConsume returns the latest data.
 *  5. Empty destinations: tryConsume returns true with an empty vector.
 */

#include "TopologyState.h"
#include <juce_core/juce_core.h>

class TopologyStateTests : public juce::UnitTest
{
public:
    TopologyStateTests() : UnitTest ("TopologyStateTests") {}

    void runTest() override
    {
        beginTest ("1. Initial state: dirty=false, tryConsume returns false");
        testInitialState();

        beginTest ("2. requestChange sets dirty; tryConsume returns true with data");
        testRequestChangeSetsFlag();

        beginTest ("3. tryConsume clears dirty; second call returns false");
        testConsumeClears();

        beginTest ("4. Multiple requestChange calls — latest data is returned");
        testMultipleRequests();

        beginTest ("5. Empty destinations");
        testEmptyDestinations();

        beginTest ("6. (#5 problem 1) request arriving between copy and clear is not lost");
        testRequestDuringConsumeIsNotLost();

        beginTest ("7. (#5 problem 3) a second consumer must not steal a pending request");
        testSecondConsumerDoesNotStealRequest();
    }

private:
    // -----------------------------------------------------------------------
    void testInitialState()
    {
        TopologyState ts;
        expect (! ts.dirty.load(), "dirty should be false on construction");

        std::vector<std::string> out;
        expect (! ts.tryConsume (out), "tryConsume should return false when not dirty");
        expect (out.empty(), "out should remain empty when nothing consumed");
    }

    // -----------------------------------------------------------------------
    void testRequestChangeSetsFlag()
    {
        TopologyState ts;
        ts.requestChange ({"H", "LH", "LLH"});

        expect (ts.dirty.load(), "dirty should be true after requestChange");

        std::vector<std::string> out;
        expect (ts.tryConsume (out), "tryConsume should return true after requestChange");

        expectEquals (static_cast<int> (out.size()), 3, "Should have 3 destinations");
        expectEquals (out[0], std::string {"H"},   "out[0] should be H");
        expectEquals (out[1], std::string {"LH"},  "out[1] should be LH");
        expectEquals (out[2], std::string {"LLH"}, "out[2] should be LLH");
    }

    // -----------------------------------------------------------------------
    void testConsumeClears()
    {
        TopologyState ts;
        ts.requestChange ({"H", "L"});

        std::vector<std::string> out;
        expect (ts.tryConsume (out), "First consume should succeed");

        // Second consume: dirty is now false
        std::vector<std::string> out2;
        expect (! ts.tryConsume (out2), "Second tryConsume should return false (already consumed)");
        expect (! ts.dirty.load(),      "dirty should be false after consume");
    }

    // -----------------------------------------------------------------------
    void testMultipleRequests()
    {
        TopologyState ts;
        ts.requestChange ({"H", "LH"});
        ts.requestChange ({"HL", "HH", "LH", "LL"});  // overwrites previous

        std::vector<std::string> out;
        expect (ts.tryConsume (out), "tryConsume should succeed");
        expectEquals (static_cast<int> (out.size()), 4, "Should have latest 4 destinations");
        expectEquals (out[0], std::string {"HL"}, "out[0] should be HL");
        expectEquals (out[1], std::string {"HH"}, "out[1] should be HH");
        expectEquals (out[2], std::string {"LH"}, "out[2] should be LH");
        expectEquals (out[3], std::string {"LL"}, "out[3] should be LL");
    }

    // -----------------------------------------------------------------------
    void testEmptyDestinations()
    {
        TopologyState ts;
        ts.requestChange ({});

        std::vector<std::string> out;
        out.push_back ("existing");  // make sure it is cleared

        expect (ts.tryConsume (out), "tryConsume with empty destinations should return true");
        expect (out.empty(), "out should be empty after consuming empty destinations");
    }

    // -----------------------------------------------------------------------
    // Issue #5 problem 1: the audio thread copied request A, then the GUI thread posts request B,
    // then the audio thread clears the dirty flag. B must still be delivered.
    void testRequestDuringConsumeIsNotLost()
    {
        TopologyState ts;
        ts.afterCopyHookForTest = [&ts] { ts.requestChange ({"HL", "HH", "LH", "LL"}); };
        ts.requestChange ({"H", "LH", "LL"});

        std::vector<std::string> first;
        expect (ts.tryConsume (first), "Request A should be consumed");
        expectEquals (static_cast<int> (first.size()), 3, "A has 3 destinations");

        ts.afterCopyHookForTest = nullptr;

        std::vector<std::string> second;
        expect (ts.tryConsume (second), "Request B posted during the consume must still be pending");
        expectEquals (static_cast<int> (second.size()), 4, "B has 4 destinations");
    }

    // -----------------------------------------------------------------------
    // Issue #5 problem 3: setStateInformation posts a request; the editor (GraphicEqWidget) consumed it
    // before the audio thread did, so the engine never received the restored topology.
    void testSecondConsumerDoesNotStealRequest()
    {
        TopologyState ts;
        ts.requestChange ({"H", "LH", "LL"});

        std::vector<std::string> editorView;
        (void) ts.tryConsume (editorView);   // what the editor did when it opened

        std::vector<std::string> audioView;
        expect (ts.tryConsume (audioView), "The audio thread must still receive the restored topology");
    }
};

// Static registration
static TopologyStateTests topologyStateTests;
