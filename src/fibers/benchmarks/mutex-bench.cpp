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
BENCHMARK_DEFINE_F(FiberMutexBench, Uncontended)(benchmark::State & state)
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
BENCHMARK_REGISTER_F(FiberMutexBench, Uncontended)->UseRealTime();

// Measures the contended handoff cost: two fibers compete for the same mutex.
// A driver fiber runs the benchmark loop while a contender fiber locks and
// unlocks as fast as possible. Each iteration = one lock acquisition under
// contention + one unlock.
BENCHMARK_DEFINE_F(FiberMutexBench, Contended)(benchmark::State & state)
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
BENCHMARK_REGISTER_F(FiberMutexBench, Contended)->UseRealTime();

// Uncontended shared fast path: lock_shared + unlock_shared with no other fiber.
BENCHMARK_DEFINE_F(FiberMutexBench, UncontendedShared)(benchmark::State & state)
{
    struct Params
    {
        benchmark::State * state;
        FiberMutex * mutex;

        static int fiberMain(Params * p) noexcept
        {
            for (auto _ : *p->state)
            {
                p->mutex->lock_shared();
                p->mutex->unlock_shared();
            }
            return 0;
        }
    };

    FiberMutex mutex;
    int r = FiberScheduler::run(Params::fiberMain, {&state, &mutex});
    SILK_ASSERT(!r);
}
BENCHMARK_REGISTER_F(FiberMutexBench, UncontendedShared)->UseRealTime();

// Two readers contending on the counter word: no mutual exclusion, just CAS
// traffic against another concurrent reader on the same cache line.
BENCHMARK_DEFINE_F(FiberMutexBench, ContendedSharedShared)(benchmark::State & state)
{
    struct Driver
    {
        benchmark::State * state;
        FiberMutex * mutex;

        static int fiberMain(Driver * p) noexcept
        {
            for (auto _ : *p->state)
            {
                p->mutex->lock_shared();
                p->mutex->unlock_shared();
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
                p->mutex->lock_shared();
                p->mutex->unlock_shared();
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
BENCHMARK_REGISTER_F(FiberMutexBench, ContendedSharedShared)->UseRealTime();

// Reader vs. writer contention: driver takes shared, contender takes exclusive.
// Each iteration exercises a true block-and-wake on whichever side currently
// holds the lock.
BENCHMARK_DEFINE_F(FiberMutexBench, ContendedSharedExclusive)(benchmark::State & state)
{
    struct Driver
    {
        benchmark::State * state;
        FiberMutex * mutex;

        static int fiberMain(Driver * p) noexcept
        {
            for (auto _ : *p->state)
            {
                p->mutex->lock_shared();
                p->mutex->unlock_shared();
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
BENCHMARK_REGISTER_F(FiberMutexBench, ContendedSharedExclusive)->UseRealTime();

} // namespace silk
