/**
 * @file TripleBufferTests.cpp
 * @brief JUCE UnitTest suite for TripleBuffer / DisplayConsumerCount.
 *
 * Covers the single-threaded hand-over semantics deterministically and the
 * concurrent behaviour with a stress test (meaningful under ThreadSanitizer).
 */

#include "TripleBuffer.h"
#include "DisplayConsumerCount.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <thread>

class TripleBufferTests : public juce::UnitTest
{
public:
    TripleBufferTests() : UnitTest ("TripleBufferTests") {}

    void runTest() override
    {
        beginTest ("Nothing to acquire before the first publish");
        {
            TripleBuffer<int> buffer;
            expect (! buffer.acquire());
            expectEquals (buffer.getReadBuffer(), 0);
        }

        beginTest ("Published value is acquired once");
        {
            TripleBuffer<int> buffer;
            buffer.getWriteBuffer() = 42;
            buffer.publish();

            expect (buffer.acquire());
            expectEquals (buffer.getReadBuffer(), 42);
            expect (! buffer.acquire(), "A value must be reported only once");
            expectEquals (buffer.getReadBuffer(), 42, "Read buffer keeps the last value");
        }

        beginTest ("Only the latest of several publishes is delivered");
        {
            TripleBuffer<int> buffer;
            for (int v = 1; v <= 10; ++v)
            {
                buffer.getWriteBuffer() = v;
                buffer.publish();
            }

            expect (buffer.acquire());
            expectEquals (buffer.getReadBuffer(), 10);
            expect (! buffer.acquire());
        }

        beginTest ("A buffer held by the reader is never written by the writer");
        {
            TripleBuffer<int> buffer;
            buffer.getWriteBuffer() = 1;
            buffer.publish();
            expect (buffer.acquire());

            const int* held = &buffer.getReadBuffer();
            for (int v = 2; v <= 20; ++v)
            {
                expect (&buffer.getWriteBuffer() != held, "Writer must never get the reader's buffer");
                buffer.getWriteBuffer() = v;
                buffer.publish();
                expectEquals (*held, 1, "Reader's buffer must not change while held");
            }

            expect (buffer.acquire());
            expectEquals (buffer.getReadBuffer(), 20);
        }

        beginTest ("A value published after the reader's acquire is not lost");
        {
            TripleBuffer<int> buffer;
            buffer.getWriteBuffer() = 1;
            buffer.publish();
            expect (buffer.acquire());

            buffer.getWriteBuffer() = 2;
            buffer.publish();

            expect (buffer.acquire(), "The newer value must still be reported");
            expectEquals (buffer.getReadBuffer(), 2);
        }

        beginTest ("Concurrent writer/reader: payload is never torn and never goes backwards");
        {
            struct Payload
            {
                std::array<int, 64> values {};
            };

            TripleBuffer<Payload> buffer;
            constexpr int kPublishes = 200000;
            std::atomic<bool> done { false };

            std::thread writer ([&]
            {
                for (int v = 1; v <= kPublishes; ++v)
                {
                    buffer.getWriteBuffer().values.fill (v);
                    buffer.publish();
                }
                done.store (true, std::memory_order_release);
            });

            int torn = 0, backwards = 0, last = 0;
            auto check = [&]
            {
                const auto& values = buffer.getReadBuffer().values;
                for (int x : values)
                    if (x != values[0])
                        ++torn;
                if (values[0] < last)
                    ++backwards;
                last = values[0];
            };

            while (! done.load (std::memory_order_acquire))
                if (buffer.acquire())
                    check();

            writer.join();
            if (buffer.acquire())
                check();

            expectEquals (torn, 0);
            expectEquals (backwards, 0);
            expectEquals (last, kPublishes, "The last published value must be delivered");
        }

        beginTest ("DisplayConsumerCount tracks scoped consumers");
        {
            DisplayConsumerCount count;
            expect (! count.isActive());
            {
                DisplayConsumerCount::Scoped first (count);
                expect (count.isActive());
                {
                    DisplayConsumerCount::Scoped second (count);
                    expect (count.isActive());
                }
                expect (count.isActive(), "Still active while one consumer remains");
            }
            expect (! count.isActive());
        }
    }
};

static TripleBufferTests tripleBufferTests;
