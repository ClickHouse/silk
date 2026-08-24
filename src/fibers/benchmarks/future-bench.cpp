#include <silk/fibers/future.h>

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/tsc.h>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

#include <sched.h>

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
BENCHMARK_REGISTER_F(FiberFutureBench, SetAllWake)->Arg(8)->Arg(64)->Arg(512)->UseRealTime();

// One-in-flight ping-pong: the caller sets ping and waits pong, the echo fiber waits ping and sets pong,
// so each iteration is one round trip - two single-fiber wakes. The variants place the pair's home
// processors (a fiber homes where its first enqueue runs) to isolate the wake paths; the parked variant
// times the round trip alone behind a pre-sleep that lets the peer's processor park.
struct PingPong
{
    /** The caller-fired future the echo fiber waits; kStopWaiter ends the echo. */
    FiberFuture ping;

    /** The echo-fired future the caller waits. */
    FiberFuture pong;

    /** The timing loop, driven from the caller fiber. */
    benchmark::State * state = nullptr;

    /** Nanoseconds the caller sleeps before each ping - lets the peer's processor park; zero stays tight. */
    uint64_t parkNs = 0;

    /** Spin on pong instead of parking - the caller's processor stays busy, so no steal rescues the wake. */
    bool spinPong = false;

    /** Per-iteration round-trip samples, caller-owned; null skips sampling. */
    uint64_t * sampleNs = nullptr;
    uint64_t sampleCapacity = 0;
    uint64_t sampleCount = 0;

    static int echoFiberMain(PingPong ** parameters) noexcept
    {
        PingPong * pingPong = *parameters;
        for (;;)
        {
            int r = pingPong->ping.wait();
            pingPong->ping.reset();
            if (r == kStopWaiter)
            {
                return 0;
            }

            pingPong->pong.set(0);
        }
    }

    static int callFiberMain(PingPong ** parameters) noexcept
    {
        PingPong * pingPong = *parameters;
        for (auto _ : *pingPong->state)
        {
            if (pingPong->parkNs)
            {
                FiberScheduler::sleep(pingPong->parkNs);
            }

            uint64_t before = Tsc::getCycles();
            pingPong->ping.set(0);

            if (pingPong->spinPong)
            {
                int pongResult;
                while (!pingPong->pong.isSet(&pongResult))
                {
                    cpuPause();
                }
            }

            pingPong->pong.wait();
            pingPong->pong.reset();
            uint64_t elapsedNs = Tsc::cyclesToNanoseconds(Tsc::getCycles() - before);
            pingPong->state->SetIterationTime(elapsedNs / 1'000'000'000.);

            if (pingPong->sampleNs && pingPong->sampleCount < pingPong->sampleCapacity)
            {
                pingPong->sampleNs[pingPong->sampleCount] = elapsedNs;
                ++pingPong->sampleCount;
            }
        }

        pingPong->ping.set(kStopWaiter);
        return 0;
    }
};

// Spawn a fiber whose first enqueue runs on the given CPU, which becomes its home processor.
static void runPingPongFiber(uint32_t cpu, int (*fiberMain)(PingPong **) noexcept, PingPong * pingPong, FiberFuture * done) noexcept
{
    std::thread spawner(
        [cpu, fiberMain, pingPong, done]() noexcept
        {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(cpu, &mask);
            int r = sched_setaffinity(0, sizeof(cpu_set_t), &mask);
            SILK_ASSERT(r == 0);

            r = FiberScheduler::run(fiberMain, static_cast<PingPong *>(pingPong), done);
            SILK_ASSERT(r == 0);
        });
    spawner.join();
}

// Drive one ping-pong pair to completion: the caller runs the timing loop, the stop-wake ends the echo.
// With samples, the percentiles land in the counters - the parked mix is bimodal, so the mean lies.
static void runPingPong(
    benchmark::State * state,
    uint32_t callCpu,
    uint32_t echoCpu,
    uint64_t parkNs,
    bool spinPong = false,
    std::vector<uint64_t> * samples = nullptr) noexcept
{
    PingPong pingPong;
    pingPong.state = state;
    pingPong.parkNs = parkNs;
    pingPong.spinPong = spinPong;
    if (samples)
    {
        pingPong.sampleNs = samples->data();
        pingPong.sampleCapacity = samples->size();
    }

    FiberFuture echoDone;
    FiberFuture callDone;
    runPingPongFiber(echoCpu, PingPong::echoFiberMain, &pingPong, &echoDone);
    runPingPongFiber(callCpu, PingPong::callFiberMain, &pingPong, &callDone);

    callDone.wait();
    echoDone.wait();

    if (samples)
    {
        std::sort(samples->begin(), samples->begin() + pingPong.sampleCount);
        state->counters["p10_ns"] = static_cast<double>((*samples)[pingPong.sampleCount / 10]);
        state->counters["p50_ns"] = static_cast<double>((*samples)[pingPong.sampleCount / 2]);
        state->counters["p90_ns"] = static_cast<double>((*samples)[pingPong.sampleCount * 9 / 10]);
        state->counters["p99_ns"] = static_cast<double>((*samples)[pingPong.sampleCount * 99 / 100]);
    }
}

// Both fibers home on one processor: a wake is a local ready-queue push and the processor never idles -
// the queue-and-switch floor with no doorbell and no park.
BENCHMARK_DEFINE_F(FiberFutureBench, PingPongSameCpu)(benchmark::State & state)
{
    runPingPong(&state, 0, 0, 0);
}
BENCHMARK_REGISTER_F(FiberFutureBench, PingPongSameCpu)->UseManualTime();

// The pair homes on two processors and the round trip is far under the spin threshold, so every wake lands
// in the peer's spin window - the cross-CPU busy path.
BENCHMARK_DEFINE_F(FiberFutureBench, PingPongCrossCpu)(benchmark::State & state)
{
    runPingPong(&state, 0, 1, 0);
}
BENCHMARK_REGISTER_F(FiberFutureBench, PingPongCrossCpu)->UseManualTime();

// A 200us pre-sleep idles both processors past the spin threshold into the timed park, so the ping wakes a
// parked peer - the per-hop cost of a one-in-flight pipeline; the pong's return wake is the busy path.
BENCHMARK_DEFINE_F(FiberFutureBench, PingPongCrossCpuParked)(benchmark::State & state)
{
    std::vector<uint64_t> samples(2000);
    runPingPong(&state, 0, 1, 200'000, false, &samples);
}
// The caller spins its processor through the round trip, so no idle processor steals the echo - the wake
// must unpark the peer's home: the true parked hop of a pipeline whose waker stays busy.
BENCHMARK_DEFINE_F(FiberFutureBench, PingPongCrossCpuParkedSpin)(benchmark::State & state)
{
    std::vector<uint64_t> samples(2000);
    runPingPong(&state, 0, 1, 200'000, true, &samples);
}
BENCHMARK_REGISTER_F(FiberFutureBench, PingPongCrossCpuParkedSpin)->UseManualTime()->Iterations(2000);
BENCHMARK_REGISTER_F(FiberFutureBench, PingPongCrossCpuParked)->UseManualTime()->Iterations(2000);

} // namespace silk
