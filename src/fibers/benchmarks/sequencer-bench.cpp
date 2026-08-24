#include <silk/fibers/sequencer.h>

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace silk
{

class FiberSequencerBench : public benchmark::Fixture
{
};

// Measures the uncontended fast path: increment() with no waiters registered.
BENCHMARK_DEFINE_F(FiberSequencerBench, IncrementUncontended)(benchmark::State & state)
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
BENCHMARK_REGISTER_F(FiberSequencerBench, IncrementUncontended)->UseRealTime();

// Measures the fiber-to-fiber round-trip cost: a driver fiber increments and
// waits for a reply; a responder fiber waits and increments back. Each
// iteration = two increments + two fiber suspensions.
BENCHMARK_DEFINE_F(FiberSequencerBench, RoundTrip)(benchmark::State & state)
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
BENCHMARK_REGISTER_F(FiberSequencerBench, RoundTrip)->UseRealTime();

// Fan-in: register N futures at tokens base+1..base+N while the counter sits at base (all push to the request
// queue and stay pending - no parking), then one advance(base+N) reaches them all in a single drain. Every
// future's token is <= current at classify time, so this is the case the skip-insert path targets: route
// straight to the wake list instead of insert-then-immediately-remove from the tree.
BENCHMARK_DEFINE_F(FiberSequencerBench, FanIn)(benchmark::State & state)
{
    struct Params
    {
        benchmark::State * state;
        uint64_t count;

        static int fiberMain(Params * params) noexcept
        {
            FiberSequencer sequencer;
            std::vector<FiberSequencer::Future> futures(params->count);
            uint64_t base = 0;
            for (auto _ : *params->state)
            {
                for (uint64_t i = 0; i < params->count; ++i)
                {
                    futures[i].reset();
                    sequencer.wait(base + 1 + i, &futures[i]);
                }

                base += params->count;
                bool advanced = sequencer.advance(base);
                SILK_ASSERT(advanced);
            }
            return 0;
        }
    };

    uint64_t count = static_cast<uint64_t>(state.range(0));
    int r = FiberScheduler::run(Params::fiberMain, {&state, count});
    SILK_ASSERT(!r);
}
BENCHMARK_REGISTER_F(FiberSequencerBench, FanIn)->Arg(8)->Arg(64)->Arg(512)->UseRealTime();

} // namespace silk
