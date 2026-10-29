/**
 * @file EngineHandoffTests.cpp
 * @brief JUCE UnitTest suite for EngineHandoff (lock-free engine hand-off, #3/#4/#5).
 *
 * Tests cover:
 *  1. publish -> acquire delivers the bundle once.
 *  2. Latest wins: an unconsumed candidate is destroyed by the producer when superseded.
 *  3. No lost update: a request published right after the audio thread took the previous one is delivered.
 *  4. Retired bundles are destroyed only by reclaim() (never on the retiring thread).
 *  5. Capacity: publish/acquire/retire cycles never overflow the retire ring; publish is refused instead.
 *  6. Destructor destroys pending and retired bundles.
 *  7. Producer and consumer on different threads: ordered delivery, nothing leaked.
 */

#include "EngineHandoff.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class EngineHandoffTests : public juce::UnitTest
{
public:
    EngineHandoffTests() : UnitTest ("EngineHandoffTests") {}

    void runTest() override
    {
        beginTest ("1. publish then acquire delivers the bundle once");
        testPublishAcquire();

        beginTest ("2. latest wins: superseded candidate is destroyed by the producer");
        testLatestWins();

        beginTest ("3. request published right after the previous one was taken is not lost");
        testNoLostUpdate();

        beginTest ("4. retired bundles are destroyed only by reclaim()");
        testRetireThenReclaim();

        beginTest ("5. publish/acquire/retire cycles never overflow the retire ring");
        testCapacityIsRespected();

        beginTest ("6. destructor destroys pending and retired bundles");
        testDestructorDestroysEverything();

        beginTest ("7. producer and consumer threads: ordered delivery and no leak");
        testThreads();
    }

private:
    struct Probe
    {
        explicit Probe (int seqIn) : seq (seqIn) { ++live; }
        ~Probe() { --live; }

        int seq;
        static inline std::atomic<int> live { 0 };
    };

    using Handoff = EngineHandoff<Probe>;

    static std::unique_ptr<Probe> make (int seq) { return std::make_unique<Probe> (seq); }

    void testPublishAcquire()
    {
        {
            Handoff handoff;
            expect (handoff.acquire() == nullptr, "Nothing pending initially");

            auto candidate = make (1);
            expect (handoff.publish (candidate), "publish should succeed");
            expect (candidate == nullptr, "publish takes ownership");

            Probe* const taken = handoff.acquire();
            expect (taken != nullptr && taken->seq == 1, "acquire returns the published bundle");
            expect (handoff.acquire() == nullptr, "The bundle is delivered only once");

            expect (handoff.retire (taken), "retire succeeds");
            expectEquals (static_cast<int> (handoff.reclaim()), 1, "reclaim destroys the retired bundle");
        }

        expectEquals (Probe::live.load(), 0, "No bundle leaked");
    }

    void testLatestWins()
    {
        Handoff handoff;
        auto a = make (1);
        auto b = make (2);
        expect (handoff.publish (a));
        expect (handoff.publish (b));
        expectEquals (Probe::live.load(), 1, "The superseded candidate was destroyed by the producer");

        Probe* const taken = handoff.acquire();
        expect (taken != nullptr && taken->seq == 2, "The audio thread gets the latest candidate");
        expect (handoff.acquire() == nullptr, "Only one candidate is delivered");
        expectEquals (static_cast<int> (handoff.outstanding()), 1, "Superseded candidate is no longer counted");

        handoff.retire (taken);
        handoff.reclaim();
        expectEquals (Probe::live.load(), 0, "No bundle leaked");
    }

    void testNoLostUpdate()
    {
        Handoff handoff;

        auto a = make (1);
        expect (handoff.publish (a));
        Probe* const takenA = handoff.acquire();                  // audio thread took A ...
        expect (takenA != nullptr && takenA->seq == 1);

        auto b = make (2);
        expect (handoff.publish (b));                             // ... and only now B arrives

        Probe* const takenB = handoff.acquire();
        expect (takenB != nullptr && takenB->seq == 2, "B must still be delivered after A was taken");

        handoff.retire (takenA);
        handoff.retire (takenB);
        expectEquals (static_cast<int> (handoff.reclaim()), 2, "Both bundles are reclaimed");
        expectEquals (Probe::live.load(), 0, "No bundle leaked");
    }

    void testRetireThenReclaim()
    {
        Handoff handoff;
        auto a = make (1);
        expect (handoff.publish (a));
        Probe* const taken = handoff.acquire();

        handoff.retire (taken);
        expectEquals (Probe::live.load(), 1, "retire() on the audio thread must not destroy the bundle");
        expectEquals (static_cast<int> (handoff.outstanding()), 1, "Still counted until reclaimed");

        handoff.reclaim();
        expectEquals (Probe::live.load(), 0, "reclaim() destroys it");
        expectEquals (static_cast<int> (handoff.outstanding()), 0, "No longer counted");
        expectEquals (static_cast<int> (handoff.reclaim()), 0, "Nothing left to reclaim");
    }

    void testCapacityIsRespected()
    {
        // Producer that never reclaims on its own: publish is refused once the capacity is reached,
        // and the retire ring never overflows.
        {
            Handoff handoff;
            Probe* active = nullptr;
            int published = 0;
            int refused = 0;

            for (int i = 0; i < 1000; ++i)
            {
                auto candidate = make (i);

                if (handoff.publish (candidate))
                {
                    ++published;
                }
                else
                {
                    ++refused;
                    expect (candidate != nullptr, "A refused candidate stays with the caller");
                    expectEquals (static_cast<int> (handoff.outstanding()), static_cast<int> (Handoff::kRetireCapacity),
                                  "publish is only refused at capacity");
                    handoff.reclaim();
                    expect (handoff.publish (candidate), "publish succeeds after reclaim");
                    ++published;
                }

                if (Probe* const taken = handoff.acquire())
                {
                    if (active != nullptr)
                        expect (handoff.retire (active), "retire must always have room");

                    active = taken;
                }

                expect (handoff.outstanding() <= Handoff::kRetireCapacity, "outstanding stays bounded");
            }

            expect (refused > 0, "The test must actually reach the capacity");
            expectEquals (static_cast<int> (handoff.retireOverflowCount()), 0, "The retire ring never overflowed");

            if (active != nullptr)
                handoff.retire (active);
        }

        expectEquals (Probe::live.load(), 0, "No bundle leaked");

        // Producer that reclaims every time and a consumer that takes every candidate: nothing is ever refused.
        {
            Handoff handoff;
            Probe* active = nullptr;

            for (int i = 0; i < 1000; ++i)
            {
                handoff.reclaim();
                auto candidate = make (i);
                expect (handoff.publish (candidate), "publish is never refused when the producer reclaims");

                Probe* const taken = handoff.acquire();
                expect (taken != nullptr);
                if (active != nullptr)
                    handoff.retire (active);
                active = taken;
            }

            handoff.retire (active);
            expectEquals (static_cast<int> (handoff.retireOverflowCount()), 0, "The retire ring never overflowed");
        }

        expectEquals (Probe::live.load(), 0, "No bundle leaked");
    }

    void testDestructorDestroysEverything()
    {
        {
            Handoff handoff;
            auto a = make (1);
            expect (handoff.publish (a));
            Probe* const taken = handoff.acquire();
            handoff.retire (taken);

            auto b = make (2);
            expect (handoff.publish (b));      // still pending when the hand-off goes away
            expectEquals (Probe::live.load(), 2);
        }

        expectEquals (Probe::live.load(), 0, "Pending and retired bundles are destroyed with the hand-off");
    }

    void testThreads()
    {
        constexpr int kCount = 5000;
        Handoff handoff;
        std::atomic<bool> producerDone { false };
        std::atomic<int> lastSeen { -1 };
        std::atomic<bool> outOfOrder { false };

        std::thread consumer ([&]
        {
            Probe* active = nullptr;
            int last = -1;

            auto take = [&] (Probe* taken)
            {
                if (taken->seq <= last)
                    outOfOrder.store (true);

                last = taken->seq;
                lastSeen.store (last);

                if (active != nullptr)
                    handoff.retire (active);

                active = taken;
            };

            for (;;)
            {
                if (Probe* const taken = handoff.acquire())
                {
                    take (taken);
                    continue;
                }

                if (! producerDone.load())
                    continue;

                // The producer has finished: one last look picks up the final candidate.
                if (Probe* const taken = handoff.acquire())
                    take (taken);

                break;
            }

            if (active != nullptr)
                handoff.retire (active);
        });

        for (int i = 0; i < kCount; ++i)
        {
            handoff.reclaim();
            auto candidate = make (i);

            while (! handoff.publish (candidate))
            {
                handoff.reclaim();
                std::this_thread::yield();
            }
        }

        producerDone.store (true);
        consumer.join();
        handoff.reclaim();

        expect (! outOfOrder.load(), "The consumer must never see an older bundle after a newer one");
        expectEquals (static_cast<int> (handoff.retireOverflowCount()), 0, "The retire ring never overflowed");
        expectEquals (static_cast<int> (handoff.outstanding()), 0, "Everything was reclaimed");
        expectEquals (Probe::live.load(), 0, "No bundle leaked");
        expectEquals (lastSeen.load(), kCount - 1, "The final request must be delivered");
    }
};

static EngineHandoffTests engineHandoffTests;
