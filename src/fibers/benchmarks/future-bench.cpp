#include <silk/fibers/future.h>

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

namespace silk
{

// Error value the driver wakes a waiter with to make it exit; normal wakeups use 0.
static constexpr int kStopWaiter = -1;

class FiberFutureBench : public benchmark::Fixture
{
};

// Batched wake: count waiter fibers each park on a future; the timed region calls setAll once to wake them
// all. The re-park barrier (waiting every waiter back onto its future) runs under PauseTiming, so the
// measurement is setAll's dispatch - extractWaitingFiber per future plus scheduleAll's coalesced doorbell
// (one msg_ring per distinct parked processor, one submit). Exercises real fiber wakeups, unlike the
// sequencer FanIn which registers bare futures with no fiber waiter.
BENCHMARK_DEFINE_F(FiberFutureBench, SetAllWake)(benchmark::State & state)
{
    uint64_t count = static_cast<uint64_t>(state.range(0));

    struct Waiter
    {
        FiberFuture * wake;
        FiberFuture * ready;

        static int fiberMain(Waiter * p) noexcept
        {
            for (;;)
            {
                p->ready->set(0); // re-armed and about to park
                int r = p->wake->wait();
                p->wake->reset();
                if (r == kStopWaiter)
                {
                    break;
                }
            }
            return 0;
        }
    };

    std::vector<FiberFuture> wake(count);
    std::vector<FiberFuture> ready(count);
    std::vector<FiberFuture> done(count);
    std::vector<FiberFuture *> wakePointers(count);

    for (uint64_t i = 0; i < count; ++i)
    {
        wakePointers[i] = &wake[i];
    }
    for (uint64_t i = 0; i < count; ++i)
    {
        int r = FiberScheduler::run(Waiter::fiberMain, {&wake[i], &ready[i]}, &done[i]);
        SILK_ASSERT(!r);
    }

    auto awaitAllParked = [&]() noexcept
    {
        for (uint64_t i = 0; i < count; ++i)
        {
            ready[i].wait();
            ready[i].reset();
        }
    };

    for (auto _ : state)
    {
        state.PauseTiming();
        awaitAllParked();
        state.ResumeTiming();

        FiberFuture::setAll(0, wakePointers.data(), count);
    }

    // Drain: every waiter is parked (barrier), so one stop-wake makes them all exit.
    awaitAllParked();
    FiberFuture::setAll(kStopWaiter, wakePointers.data(), count);
    for (uint64_t i = 0; i < count; ++i)
    {
        done[i].wait();
    }
}
BENCHMARK_REGISTER_F(FiberFutureBench, SetAllWake)->Arg(8)->Arg(64)->Arg(512);

} // namespace silk
