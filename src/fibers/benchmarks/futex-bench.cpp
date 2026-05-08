#include <silk/fibers/futex.h>

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>

#include <benchmark/benchmark.h>

#include <atomic>

namespace silk
{

class FiberFutexBench : public benchmark::Fixture
{
};

// Measures the fiber-to-fiber round-trip cost: a driver fiber posts a request
// and waits for a reply; a responder fiber waits for the request and posts the
// reply. Each iteration = two posts + two fiber suspensions, comparable to the
// mutex contended handoff benchmark.
BENCHMARK_F(FiberFutexBench, RoundTrip)(benchmark::State & state)
{
    struct Responder
    {
        FiberFutex * request;
        FiberFutex * reply;
        std::atomic<bool> * stop;

        static int fiberMain(Responder * p) noexcept
        {
            for (uint64_t i = 1; !p->stop->load(std::memory_order_relaxed); ++i)
            {
                int r = p->request->wait(i);
                if (r)
                {
                    return r;
                }
                if (p->stop->load(std::memory_order_relaxed))
                {
                    break;
                }
                p->reply->post();
            }
            return 0;
        }
    };

    struct Driver
    {
        benchmark::State * state;
        FiberFutex * request;
        FiberFutex * reply;

        static int fiberMain(Driver * p) noexcept
        {
            uint64_t replyToken = 1;
            for (auto _ : *p->state)
            {
                p->request->post();
                int r = p->reply->wait(replyToken);
                if (r)
                {
                    return r;
                }
                ++replyToken;
            }
            return 0;
        }
    };

    FiberFutex request, reply;
    std::atomic<bool> stop{false};

    FiberFuture responder, driver;
    int r = FiberScheduler::run(Responder::fiberMain, {&request, &reply, &stop}, &responder);
    SILK_ASSERT(!r);
    r = FiberScheduler::run(Driver::fiberMain, {&state, &request, &reply}, &driver);
    SILK_ASSERT(!r);

    driver.wait();
    stop.store(true, std::memory_order_relaxed);
    request.post();
    responder.wait();
}

} // namespace silk
