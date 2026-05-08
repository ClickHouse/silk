#include <silk/fibers/fiber.h>

#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <benchmark/benchmark.h>

#include <memory>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace silk
{

class FiberBench : public benchmark::Fixture
{
};

// Fiber creation + join: schedule a no-op fiber and block until it exits.
// Pairs with PlatformBench/ThreadCreateJoin to compare against std::thread.
BENCHMARK_F(FiberBench, RunJoin)(benchmark::State & state)
{
    struct Params
    {
        static int fiberMain(Params *) noexcept { return 0; }
    };

    for (auto _ : state)
    {
        int r = FiberScheduler::run(Params::fiberMain, {});
        SILK_ASSERT(!r);
    }
}

// Measures the round-trip cost of a fiber context switch: fiber yields to the
// scheduler, which re-enqueues it and runs it again.  Each iteration = one yield
// = two context switches (fiber => scheduler and scheduler => fiber).
BENCHMARK_F(FiberBench, ContextSwitch)(benchmark::State & state)
{
    struct Params
    {
        benchmark::State * state;

        static int fiberMain(Params * p) noexcept
        {
            for (auto _ : *p->state)
            {
                FiberScheduler::yield();
            }
            return 0;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {&state});
    SILK_ASSERT(!r);
}

// Pipe round-trip using blocking io_uring read/write.
// Each iteration = one thread => fiber => thread round trip.
BENCHMARK_F(FiberBench, PipeRoundTripBlocking)(benchmark::State & state)
{
    // reqFds: thread writes, fiber reads.
    // respFds: fiber writes, thread reads.
    int reqFds[2];
    int respFds[2];
    ::pipe(reqFds);
    ::pipe(respFds);

    struct Params
    {
        int readFd; // reqFds[0]
        int writeFd; // respFds[1]

        static int fiberMain(Params * p) noexcept
        {
            char byte = 0;
            for (;;)
            {
                uint64_t n = 0;
                int r = FiberScheduler::read(p->readFd, &byte, 1, 0, &n);
                SILK_ASSERT(r == 0);
                if (n == 0)
                {
                    // EOF: thread closed its write end
                    break;
                }
                r = FiberScheduler::write(p->writeFd, &byte, 1, 0);
                SILK_ASSERT(r == 0);
            }
            return 0;
        }
    };

    FiberFuture future;
    int r = FiberScheduler::run(Params::fiberMain, {reqFds[0], respFds[1]}, &future);
    SILK_ASSERT(!r);

    char byte = 0;
    for (auto _ : state)
    {
        ssize_t w = ::write(reqFds[1], &byte, 1);
        SILK_ASSERT(w == 1);
        ssize_t n = ::read(respFds[0], &byte, 1);
        SILK_ASSERT(n == 1);
    }

    ::close(reqFds[1]); // signal EOF to fiber
    future.wait();

    ::close(reqFds[0]);
    ::close(respFds[0]);
    ::close(respFds[1]);
}

// Pipe round-trip using non-blocking syscalls with poll() fallback.
// Each iteration = one thread => fiber => thread round trip.
BENCHMARK_F(FiberBench, PipeRoundTripNonBlocking)(benchmark::State & state)
{
    int reqFds[2];
    int respFds[2];
    ::pipe(reqFds);
    ::pipe(respFds);

    // Set the fiber-facing ends to non-blocking.
    ::fcntl(reqFds[0], F_SETFL, O_NONBLOCK);
    ::fcntl(respFds[1], F_SETFL, O_NONBLOCK);

    struct Params
    {
        int readFd; // reqFds[0]
        int writeFd; // respFds[1]

        static int fiberMain(Params * p) noexcept
        {
            char byte = 0;
            for (;;)
            {
                ssize_t n;
                while ((n = ::read(p->readFd, &byte, 1)) < 0)
                {
                    SILK_ASSERT(errno == EAGAIN);
                    int r = FiberScheduler::poll(p->readFd, POLLIN);
                    SILK_ASSERT(r == 0);
                }
                if (n == 0)
                {
                    // EOF: thread closed its write end
                    break;
                }
                while (::write(p->writeFd, &byte, 1) < 0)
                {
                    SILK_ASSERT(errno == EAGAIN);
                    int r = FiberScheduler::poll(p->writeFd, POLLOUT);
                    SILK_ASSERT(r == 0);
                }
            }
            return 0;
        }
    };

    FiberFuture future;
    int r = FiberScheduler::run(Params::fiberMain, {reqFds[0], respFds[1]}, &future);
    SILK_ASSERT(!r);

    char byte = 0;
    for (auto _ : state)
    {
        ssize_t w = ::write(reqFds[1], &byte, 1);
        SILK_ASSERT(w == 1);
        ssize_t n = ::read(respFds[0], &byte, 1);
        SILK_ASSERT(n == 1);
    }

    ::close(reqFds[1]); // signal EOF to fiber
    future.wait();

    ::close(reqFds[0]);
    ::close(respFds[0]);
    ::close(respFds[1]);
}

// io_uring fiber ping-pong: two fibers exchange bytes through a pipe, both
// using io_uring for reads. Each iteration = one full round-trip = two genuine
// fiber suspensions on io_uring read CQEs, with no OS thread scheduling in
// the hot path. Measures: SQE submit -> fiber suspend -> CQE -> fiber resume.
BENCHMARK_F(FiberBench, IoUringFiberPingPong)(benchmark::State & state)
{
    int pingFds[2];
    int pongFds[2];
    ::pipe(pingFds);
    ::pipe(pongFds);

    struct Pong
    {
        int readFd;
        int writeFd;

        static int fiberMain(Pong * p) noexcept
        {
            char byte = 0;
            for (;;)
            {
                uint64_t n = 0;
                int r = FiberScheduler::read(p->readFd, &byte, 1, 0, &n);
                SILK_ASSERT(r == 0);
                if (n == 0)
                {
                    // EOF: thread closed its write end
                    break;
                }
                r = FiberScheduler::write(p->writeFd, &byte, 1, 0);
                SILK_ASSERT(r == 0);
            }
            return 0;
        }
    };

    struct Ping
    {
        benchmark::State * state;
        int writeFd;
        int readFd;

        static int fiberMain(Ping * p) noexcept
        {
            char byte = 0;
            for (auto _ : *p->state)
            {
                int r = FiberScheduler::write(p->writeFd, &byte, 1, 0);
                SILK_ASSERT(r == 0);
                uint64_t n = 0;
                r = FiberScheduler::read(p->readFd, &byte, 1, 0, &n);
                SILK_ASSERT(r == 0);
            }
            return 0;
        }
    };

    FiberFuture pong;
    int r = FiberScheduler::run(Pong::fiberMain, {pingFds[0], pongFds[1]}, &pong);
    SILK_ASSERT(!r);

    FiberFuture ping;
    r = FiberScheduler::run(Ping::fiberMain, {&state, pingFds[1], pongFds[0]}, &ping);
    SILK_ASSERT(!r);
    ping.wait();

    ::close(pingFds[1]);
    pong.wait();

    ::close(pingFds[0]);
    ::close(pongFds[0]);
    ::close(pongFds[1]);
}

// Sleep round-trip: submit a 0ns async sleep and wait for it to expire.
// Each iteration exercises sleep submission, handleSleepQueue expiry, and
// fiber wakeup without any real wall-clock delay.
BENCHMARK_F(FiberBench, Sleep)(benchmark::State & state)
{
    struct Params
    {
        benchmark::State * state;

        static int fiberMain(Params * p) noexcept
        {
            for (auto _ : *p->state)
            {
                FiberScheduler::sleep(0);
            }
            return 0;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {&state});
    SILK_ASSERT(!r);
}

// Sleep wakeup latency: measures real elapsed time per 100us sleep, exercising
// the full io_uring timeout path (submission -> eventfd wakeup -> expiry).
// Reports actual elapsed time via manual timing; overhead above 100us is the
// wakeup latency.
BENCHMARK_DEFINE_F(FiberBench, SleepWakeup)(benchmark::State & state)
{
    static constexpr uint64_t SLEEP_NS = 100'000; // 100us

    struct Params
    {
        benchmark::State * state;

        static int fiberMain(Params * p) noexcept
        {
            for (auto _ : *p->state)
            {
                uint64_t before = Tsc::getCycles();
                FiberScheduler::sleep(SLEEP_NS);
                uint64_t elapsedNs = Tsc::cyclesToNanoseconds(Tsc::getCycles() - before);
                p->state->SetIterationTime(elapsedNs / 1'000'000'000.);
            }
            return 0;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {&state});
    SILK_ASSERT(!r);
}
BENCHMARK_REGISTER_F(FiberBench, SleepWakeup)->UseManualTime();

// Sleep cancel: submit a 60s async sleep and immediately cancel it (cancel-
// before-insert path).  Measures cancel + handleSleepQueue ECANCELED delivery.
BENCHMARK_F(FiberBench, SleepCancel)(benchmark::State & state)
{
    static constexpr uint64_t SLEEP_NS = 60'000'000'000; // 60s

    struct Params
    {
        benchmark::State * state;

        static int fiberMain(Params * p) noexcept
        {
            for (auto _ : *p->state)
            {
                FiberScheduler::SleepFuture future;
                FiberScheduler::sleep(SLEEP_NS, &future);
                future.cancel();
                future.wait();
            }
            return 0;
        }
    };

    int r = FiberScheduler::run(Params::fiberMain, {&state});
    SILK_ASSERT(!r);
}

// Work-stealing throughput: main thread schedules fibers that land on its CPU;
// idle scheduler threads steal and execute them. Maintains a ring of N in-flight
// fibers so the steal loop always has work available. Each iteration = one join
// (oldest fiber) + one schedule (new fiber). N=1 is the serial baseline with no
// overlap; larger N fills the steal pipeline across CPUs.
BENCHMARK_DEFINE_F(FiberBench, WorkStealingThreadProducer)(benchmark::State & state)
{
    struct Params
    {
        static int fiberMain(Params *) noexcept { return 0; }
    };

    uint64_t numberOfFibers = static_cast<uint64_t>(state.range(0));

    auto futures = std::make_unique<FiberFuture[]>(numberOfFibers);
    for (uint64_t i = 0; i < numberOfFibers; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {}, &futures[i]);
        SILK_ASSERT(!r);
    }

    uint64_t pos = 0;
    for (auto _ : state)
    {
        futures[pos % numberOfFibers].wait();
        futures[pos % numberOfFibers].reset();
        int r = FiberScheduler::run(Params::fiberMain, {}, &futures[pos % numberOfFibers]);
        SILK_ASSERT(!r);
        ++pos;
    }

    for (uint64_t i = 0; i < numberOfFibers; ++i)
    {
        futures[(pos + i) % numberOfFibers].wait();
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_REGISTER_F(FiberBench, WorkStealingThreadProducer)->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(256);

// Same ring benchmark but the loop runs inside a driver fiber so child
// completion is delivered via FiberFuture rather than a POSIX semaphore.
// Eliminates the semaphore syscall on the hot path; wait/wake goes through
// the scheduler's own suspend/schedule mechanism.
BENCHMARK_DEFINE_F(FiberBench, WorkStealingFiberProducer)(benchmark::State & state)
{
    struct Child
    {
        uint64_t spinCount;

        static int fiberMain(Child * p) noexcept
        {
            for (uint64_t i = 0; i < p->spinCount; ++i)
            {
                cpuPause();
            }
            return 0;
        }
    };

    struct Driver
    {
        benchmark::State * state;
        uint64_t numberOfFibers;
        uint64_t spinCount;

        static int fiberMain(Driver * p) noexcept
        {
            auto futures = std::make_unique<FiberFuture[]>(p->numberOfFibers);
            for (uint64_t i = 0; i < p->numberOfFibers; ++i)
            {
                int r = FiberScheduler::run(Child::fiberMain, {p->spinCount}, &futures[i]);
                SILK_ASSERT(!r);
            }

            uint64_t pos = 0;
            for (auto _ : *p->state)
            {
                futures[pos % p->numberOfFibers].wait();
                futures[pos % p->numberOfFibers].reset();
                int r = FiberScheduler::run(Child::fiberMain, {p->spinCount}, &futures[pos % p->numberOfFibers]);
                SILK_ASSERT(!r);
                ++pos;
            }

            for (uint64_t i = 0; i < p->numberOfFibers; ++i)
            {
                futures[(pos + i) % p->numberOfFibers].wait();
            }

            return 0;
        }
    };

    uint64_t numberOfFibers = static_cast<uint64_t>(state.range(0));
    uint64_t spinCount = static_cast<uint64_t>(state.range(1));

    int r = FiberScheduler::run(Driver::fiberMain, {&state, numberOfFibers, spinCount});
    SILK_ASSERT(!r);

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_REGISTER_F(FiberBench, WorkStealingFiberProducer)
    ->Args({1, 0})
    ->Args({16, 0})
    ->Args({1, 100})
    ->Args({16, 100})
    ->Args({1, 1000})
    ->Args({16, 1000})
    ->Args({1, 10000})
    ->Args({16, 10000});

} // namespace silk
