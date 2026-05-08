#include <silk/fibers/sequencer.h>

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>

#include <benchmark/benchmark.h>

#include <atomic>

namespace silk
{

class FiberSequencerBench : public benchmark::Fixture
{
};

// Measures the uncontended fast path: increment() with no waiters registered.
BENCHMARK_F(FiberSequencerBench, IncrementUncontended)(benchmark::State & state)
{
    struct Params
    {
        benchmark::State * state;
        FiberSequencer * sequencer;

        static int fiberMain(Params * p) noexcept
        {
            for (auto _ : *p->state)
            {
                p->sequencer->increment();
            }
            return 0;
        }
    };

    FiberSequencer sequencer;
    int r = FiberScheduler::run(Params::fiberMain, {&state, &sequencer});
    SILK_ASSERT(!r);
}

// Measures the fiber-to-fiber round-trip cost: a driver fiber increments and
// waits for a reply; a responder fiber waits and increments back. Each
// iteration = two increments + two fiber suspensions.
BENCHMARK_F(FiberSequencerBench, RoundTrip)(benchmark::State & state)
{
    struct Responder
    {
        FiberSequencer * request;
        FiberSequencer * reply;
        std::atomic<bool> * stop;

        static int fiberMain(Responder * p) noexcept
        {
            for (uint64_t i = 1; !p->stop->load(std::memory_order_relaxed); ++i)
            {
                p->request->wait(i);
                if (p->stop->load(std::memory_order_relaxed))
                {
                    break;
                }
                p->reply->increment();
            }
            return 0;
        }
    };

    struct Driver
    {
        benchmark::State * state;
        FiberSequencer * request;
        FiberSequencer * reply;

        static int fiberMain(Driver * p) noexcept
        {
            uint64_t replyToken = 1;
            for (auto _ : *p->state)
            {
                p->request->increment();
                p->reply->wait(replyToken);
                ++replyToken;
            }
            return 0;
        }
    };

    FiberSequencer request, reply;
    std::atomic<bool> stop{false};

    FiberFuture responder, driver;
    int r = FiberScheduler::run(Responder::fiberMain, {&request, &reply, &stop}, &responder);
    SILK_ASSERT(!r);
    r = FiberScheduler::run(Driver::fiberMain, {&state, &request, &reply}, &driver);
    SILK_ASSERT(!r);

    driver.wait();
    stop.store(true, std::memory_order_relaxed);
    request.increment();
    responder.wait();
}

} // namespace silk
