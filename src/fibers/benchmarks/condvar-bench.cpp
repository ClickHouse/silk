#include <silk/fibers/condvar.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/mutex.h>
#include <silk/util/assert.h>

#include <benchmark/benchmark.h>

#include <atomic>

namespace silk
{

class FiberCondVarBench : public benchmark::Fixture
{
};

// Measures the uncontended fast path of notify_one(): the combiner enters,
// finds the waiter list empty, and exits. The counter increment is the only
// real work done. No waiter is ever registered.
BENCHMARK_F(FiberCondVarBench, NotifyOneUncontended)(benchmark::State & state)
{
    struct Params
    {
        benchmark::State * state;
        FiberCondVar * cv;

        static int fiberMain(Params * p) noexcept
        {
            for (auto _ : *p->state)
            {
                p->cv->notify_one();
            }
            return 0;
        }
    };

    FiberCondVar cv;
    int r = FiberScheduler::run(Params::fiberMain, {&state, &cv});
    SILK_ASSERT(!r);
}

// Same as above for notify_all(): the sticky notifyAll flag is set, the
// combiner observes an empty waiter list and clears the flag.
BENCHMARK_F(FiberCondVarBench, NotifyAllUncontended)(benchmark::State & state)
{
    struct Params
    {
        benchmark::State * state;
        FiberCondVar * cv;

        static int fiberMain(Params * p) noexcept
        {
            for (auto _ : *p->state)
            {
                p->cv->notify_all();
            }
            return 0;
        }
    };

    FiberCondVar cv;
    int r = FiberScheduler::run(Params::fiberMain, {&state, &cv});
    SILK_ASSERT(!r);
}

// Producer/consumer ping-pong: a driver fiber publishes an item under the
// mutex and notifies; a responder fiber waits, consumes the item, and signals
// the driver to continue. Each iteration = two mutex round-trips + one
// notify_one drain that wakes a waiter + one notify_one drain that exits empty
// (the driver's wait sees the responder's "consumed" signal immediately).
BENCHMARK_F(FiberCondVarBench, RoundTrip)(benchmark::State & state)
{
    struct Shared
    {
        FiberCondVar produced;
        FiberCondVar consumed;
        FiberMutex mutex;
        int items = 0;
        std::atomic<bool> stop{false};
    };

    struct Responder
    {
        Shared * shared;

        static int fiberMain(Responder * p) noexcept
        {
            Shared * s = p->shared;
            while (true)
            {
                s->mutex.lock();
                while (s->items == 0 && !s->stop.load(std::memory_order_relaxed))
                {
                    s->produced.wait(s->mutex);
                }
                if (s->stop.load(std::memory_order_relaxed))
                {
                    s->mutex.unlock();
                    return 0;
                }
                --s->items;
                s->mutex.unlock();
                s->consumed.notify_one();
            }
        }
    };

    struct Driver
    {
        benchmark::State * state;
        Shared * shared;

        static int fiberMain(Driver * p) noexcept
        {
            Shared * s = p->shared;
            for (auto _ : *p->state)
            {
                s->mutex.lock();
                ++s->items;
                s->mutex.unlock();
                s->produced.notify_one();

                s->mutex.lock();
                while (s->items != 0)
                {
                    s->consumed.wait(s->mutex);
                }
                s->mutex.unlock();
            }
            return 0;
        }
    };

    Shared shared;
    FiberFuture responder, driver;
    int r = FiberScheduler::run(Responder::fiberMain, {&shared}, &responder);
    SILK_ASSERT(!r);
    r = FiberScheduler::run(Driver::fiberMain, {&state, &shared}, &driver);
    SILK_ASSERT(!r);

    driver.wait();

    shared.mutex.lock();
    shared.stop.store(true, std::memory_order_relaxed);
    shared.mutex.unlock();
    shared.produced.notify_one();
    responder.wait();
}

// Broadcast wake-up cost: N waiter fibers suspend on cv.wait(); the driver
// fires notify_all() and waits for every waiter to retire, then the cycle
// repeats. Each iteration = N waiters suspending + one notify_all that wakes
// them all + the driver re-checking the count.
BENCHMARK_F(FiberCondVarBench, NotifyAllWakesN)(benchmark::State & state)
{
    static constexpr int N = 8;

    struct Shared
    {
        FiberCondVar startSignal;
        FiberCondVar doneSignal;
        FiberMutex mutex;
        int generation = 0;
        int doneCount = 0;
        std::atomic<bool> stop{false};
    };

    struct Waiter
    {
        Shared * shared;

        static int fiberMain(Waiter * p) noexcept
        {
            Shared * s = p->shared;
            int seen = 0;
            while (true)
            {
                s->mutex.lock();
                while (s->generation == seen && !s->stop.load(std::memory_order_relaxed))
                {
                    s->startSignal.wait(s->mutex);
                }
                if (s->stop.load(std::memory_order_relaxed))
                {
                    s->mutex.unlock();
                    return 0;
                }
                seen = s->generation;
                ++s->doneCount;
                s->mutex.unlock();
                s->doneSignal.notify_one();
            }
        }
    };

    struct Driver
    {
        benchmark::State * state;
        Shared * shared;

        static int fiberMain(Driver * p) noexcept
        {
            Shared * s = p->shared;
            for (auto _ : *p->state)
            {
                s->mutex.lock();
                s->doneCount = 0;
                ++s->generation;
                s->mutex.unlock();
                s->startSignal.notify_all();

                s->mutex.lock();
                while (s->doneCount != N)
                {
                    s->doneSignal.wait(s->mutex);
                }
                s->mutex.unlock();
            }
            return 0;
        }
    };

    Shared shared;
    FiberFuture waiters[N], driver;
    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Waiter::fiberMain, {&shared}, &waiters[i]);
        SILK_ASSERT(!r);
    }
    int r = FiberScheduler::run(Driver::fiberMain, {&state, &shared}, &driver);
    SILK_ASSERT(!r);

    driver.wait();

    shared.mutex.lock();
    shared.stop.store(true, std::memory_order_relaxed);
    shared.mutex.unlock();
    shared.startSignal.notify_all();

    for (int i = 0; i < N; ++i)
    {
        waiters[i].wait();
    }
}

} // namespace silk
