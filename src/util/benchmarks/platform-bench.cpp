#include <silk/util/platform.h>

#include <silk/util/assert.h>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdlib>
#include <thread>

#include <sched.h>
#include <semaphore.h>
#include <unistd.h>

#include <sys/eventfd.h>

namespace silk
{

class PlatformBench : public benchmark::Fixture
{
};

BENCHMARK_F(PlatformBench, GetCurrentProcessor)(benchmark::State & state)
{
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(getCurrentProcessor());
    }
}

// Thread creation + join: spawn a no-op thread and block until it exits.
// Baseline for comparing FiberScheduler::run overhead.
BENCHMARK_F(PlatformBench, ThreadCreateJoin)(benchmark::State & state)
{
    for (auto _ : state)
    {
        std::thread thread([] { });
        thread.join();
    }
}

// sem_post followed by sem_wait on the same thread: count goes 0->1->0 with no
// waiter, so sem_wait takes the fast path (no futex syscall).
BENCHMARK_F(PlatformBench, SemPostWait)(benchmark::State & state)
{
    sem_t sem;
    sem_init(&sem, 0, 0);

    for (auto _ : state)
    {
        sem_post(&sem);
        sem_wait(&sem);
    }

    sem_destroy(&sem);
}

// Cross-thread sem round trip: benchmark thread posts, helper thread wakes and
// posts back. Measures the full futex wake + reschedule latency, which is the
// cost of join() when the fiber completes after the caller blocks.
BENCHMARK_F(PlatformBench, SemRoundTrip)(benchmark::State & state)
{
    sem_t request, response;
    sem_init(&request, 0, 0);
    sem_init(&response, 0, 0);

    std::atomic<bool> stop{false};
    std::thread helper(
        [&]
        {
            for (;;)
            {
                sem_wait(&request);
                if (stop.load(std::memory_order_relaxed))
                {
                    break;
                }
                sem_post(&response);
            }
        });

    for (auto _ : state)
    {
        sem_post(&request);
        sem_wait(&response);
    }

    stop.store(true, std::memory_order_relaxed);
    sem_post(&request);
    helper.join();

    sem_destroy(&request);
    sem_destroy(&response);
}

// eventfd_write followed by eventfd_read on the same thread: data is available
// immediately, so read returns without blocking.
BENCHMARK_F(PlatformBench, EventFdWriteRead)(benchmark::State & state)
{
    int fd = ::eventfd(0, EFD_NONBLOCK);
    SILK_ASSERT(fd >= 0);

    for (auto _ : state)
    {
        eventfd_t val;
        ::eventfd_write(fd, 1);
        ::eventfd_read(fd, &val);
    }

    ::close(fd);
}

// Cross-thread eventfd round trip: benchmark thread writes, helper thread wakes
// from a blocking read and writes back. Measures the full epoll/read wake
// latency vs the futex-based semaphore round trip.
BENCHMARK_F(PlatformBench, EventFdRoundTrip)(benchmark::State & state)
{
    int request = ::eventfd(0, 0);
    int response = ::eventfd(0, 0);
    SILK_ASSERT(request >= 0 && response >= 0);

    std::atomic<bool> stop{false};
    std::thread helper(
        [&]
        {
            for (;;)
            {
                eventfd_t val;
                ::eventfd_read(request, &val);
                if (stop.load(std::memory_order_relaxed))
                {
                    break;
                }
                ::eventfd_write(response, 1);
            }
        });

    for (auto _ : state)
    {
        eventfd_t val;
        ::eventfd_write(request, 1);
        ::eventfd_read(response, &val);
    }

    stop.store(true, std::memory_order_relaxed);
    ::eventfd_write(request, 1);
    helper.join();

    ::close(request);
    ::close(response);
}

// Cache-line ownership round trip between two cores: main thread stores an odd
// value, helper thread spins until it observes the change then stores the next
// even value, main thread spins until it observes that. One iteration = two
// ownership transfers (main -> helper, helper -> main). No syscalls involved --
// pure MESI protocol traffic.
BENCHMARK_F(PlatformBench, CacheLineRoundTrip)(benchmark::State & state)
{
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> shared{};

    std::thread helper(
        [&]
        {
            uint64_t cur = 0;
            for (;;)
            {
                uint64_t next;
                while ((next = shared.load(std::memory_order_acquire)) == cur)
                {
                    cpuPause();
                }
                if (next == UINT64_MAX)
                {
                    break;
                }
                cur = next + 1;
                shared.store(cur, std::memory_order_release);
            }
        });

    uint64_t cur = 0;
    for (auto _ : state)
    {
        shared.store(cur + 1, std::memory_order_release);
        while (shared.load(std::memory_order_acquire) == cur + 1)
        {
            cpuPause();
        }
        cur += 2;
    }

    shared.store(UINT64_MAX, std::memory_order_release);
    helper.join();
}

BENCHMARK_F(PlatformBench, CAS64)(benchmark::State & state)
{
    static std::atomic<uint64_t> val{};

    uint64_t expected = 0;
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(val.compare_exchange_weak(expected, expected + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
        ++expected;
    }
}
BENCHMARK_REGISTER_F(PlatformBench, CAS64)->ThreadRange(1, getAvailableProcessorCount());

BENCHMARK_F(PlatformBench, CAS128)(benchmark::State & state)
{
    struct alignas(16) Pair
    {
        uint64_t a;
        uint64_t b;
    };
    static std::atomic<Pair> val{};

    Pair expected{};
    for (auto _ : state)
    {
        Pair desired{expected.a + 1, expected.b + 1};
        benchmark::DoNotOptimize(val.compare_exchange_weak(expected, desired, std::memory_order_acq_rel, std::memory_order_relaxed));
        expected = desired;
    }
}
BENCHMARK_REGISTER_F(PlatformBench, CAS128)->ThreadRange(1, getAvailableProcessorCount());

BENCHMARK_F(PlatformBench, MallocFree)(benchmark::State & state)
{
    for (auto _ : state)
    {
        void * ptr = std::malloc(64);
        benchmark::DoNotOptimize(ptr);
        std::free(ptr);
    }
}
BENCHMARK_REGISTER_F(PlatformBench, MallocFree)->ThreadRange(1, getAvailableProcessorCount());

} // namespace silk
