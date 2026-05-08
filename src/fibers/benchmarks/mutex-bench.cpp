#include <silk/fibers/mutex.h>

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>

#include <benchmark/benchmark.h>

#include <atomic>

namespace silk
{

class FiberMutexBench : public benchmark::Fixture
{
};

// Measures the uncontended fast path: lock() CAS acquire + unlock() CAS release
// with no other fiber competing.
BENCHMARK_F(FiberMutexBench, Uncontended)(benchmark::State & state)
{
    struct Params
    {
        benchmark::State * state;
        FiberMutex * mutex;

        static int fiberMain(Params * p) noexcept
        {
            for (auto _ : *p->state)
            {
                p->mutex->lock();
                p->mutex->unlock();
            }
            return 0;
        }
    };

    FiberMutex mutex;
    int r = FiberScheduler::run(Params::fiberMain, {&state, &mutex});
    SILK_ASSERT(!r);
}

// Measures the contended handoff cost: two fibers compete for the same mutex.
// A driver fiber runs the benchmark loop while a contender fiber locks and
// unlocks as fast as possible. Each iteration = one lock acquisition under
// contention + one unlock.
BENCHMARK_F(FiberMutexBench, Contended)(benchmark::State & state)
{
    struct Driver
    {
        benchmark::State * state;
        FiberMutex * mutex;

        static int fiberMain(Driver * p) noexcept
        {
            for (auto _ : *p->state)
            {
                p->mutex->lock();
                p->mutex->unlock();
            }
            return 0;
        }
    };

    struct Contender
    {
        FiberMutex * mutex;
        std::atomic<bool> * stop;

        static int fiberMain(Contender * p) noexcept
        {
            while (!p->stop->load(std::memory_order_relaxed))
            {
                p->mutex->lock();
                p->mutex->unlock();
            }
            return 0;
        }
    };

    FiberMutex mutex;
    std::atomic<bool> stop{false};

    FiberFuture contender, driver;
    int r = FiberScheduler::run(Contender::fiberMain, {&mutex, &stop}, &contender);
    SILK_ASSERT(!r);
    r = FiberScheduler::run(Driver::fiberMain, {&state, &mutex}, &driver);
    SILK_ASSERT(!r);

    driver.wait();
    stop.store(true, std::memory_order_relaxed);
    contender.wait();
}

// Strict fiber-to-fiber round-trip using two mutexes as binary semaphores.
// FiberMutex does not enforce ownership in unlock(), so unlock() acts as a
// post and lock() acts as a wait. Each iteration = two unlocks + two locks,
// directly comparable to FiberFutexBench/RoundTrip.
BENCHMARK_F(FiberMutexBench, RoundTrip)(benchmark::State & state)
{
    struct Responder
    {
        FiberMutex * req;
        FiberMutex * rep;
        std::atomic<bool> * stop;

        static int fiberMain(Responder * p) noexcept
        {
            while (!p->stop->load(std::memory_order_relaxed))
            {
                p->req->lock();
                if (p->stop->load(std::memory_order_relaxed))
                {
                    break;
                }
                p->rep->unlock();
            }
            return 0;
        }
    };

    struct Driver
    {
        benchmark::State * state;
        FiberMutex * req;
        FiberMutex * rep;

        static int fiberMain(Driver * p) noexcept
        {
            for (auto _ : *p->state)
            {
                p->req->unlock();
                p->rep->lock();
            }
            return 0;
        }
    };

    FiberMutex req, rep;
    req.lock();
    rep.lock();

    std::atomic<bool> stop{false};
    FiberFuture responder, driver;
    int r = FiberScheduler::run(Responder::fiberMain, {&req, &rep, &stop}, &responder);
    SILK_ASSERT(!r);
    r = FiberScheduler::run(Driver::fiberMain, {&state, &req, &rep}, &driver);
    SILK_ASSERT(!r);

    driver.wait();
    stop.store(true, std::memory_order_relaxed);
    req.unlock();
    responder.wait();
}

} // namespace silk
