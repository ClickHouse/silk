#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/tsc.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace silk
{

// Stress harness targeting lost wakeups around sleep expiry and cancellation.
//
// The hard-to-reproduce window is the instant a sleep's deadline expires while a
// cancellation for the same SleepFuture is in flight: expiry (service loop) and
// cancel (any fiber or thread) race on the IN_TABLE/CANCELLED state and on the
// per-CPU wakeThread/parkThread handoff. A lost wakeup leaves the sleeping fiber
// suspended forever, so every test below proves liveness by counting completions
// and failing through a watchdog rather than hanging.
//
// Each cancellation is deliberately aligned to land at the sleeper's deadline
// plus a signed jitter, so cancels sweep the narrow before-expiry / at-expiry /
// after-expiry window instead of arriving uniformly at random.
struct SleepStress
{
    static constexpr uint32_t NUM_SLEEPERS = 128;
    static constexpr uint32_t NUM_CANCELLERS = 128;
    static constexpr uint32_t NUM_WAITERS = 128;
    static constexpr uint32_t NUM_CANCEL_THREADS = 8;

    static constexpr uint64_t DEFAULT_ITEM_COUNT = 50'000;
    static constexpr uint64_t DURATION_MIN_NS = 40'000;
    static constexpr uint64_t DURATION_MAX_NS = 400'000;
    static constexpr uint64_t JITTER_NS = 50'000;
    static constexpr uint64_t CANCEL_PERCENT = 85;

    static constexpr uint64_t DEFAULT_WATCHDOG_SECONDS = 60;
    static constexpr uint64_t ARM_WAIT_BUDGET_NS = 10'000'000'000;

    // Sentinel for a result slot whose worker has not recorded an outcome yet.
    static constexpr int PENDING = -1;

    struct SleepItem
    {
        FiberScheduler::SleepFuture future;
        std::atomic<uint64_t> armCycles{0};
        std::atomic<int> result{PENDING};
        uint64_t durationNs{0};
        int64_t targetOffsetCycles{0};
        bool cancel{false};
    };

    struct SleepContext
    {
        SleepItem * items{nullptr};
        uint64_t itemCount{0};
        uint64_t armWaitBudgetCycles{0};
        std::atomic<uint64_t> nextSleepIndex{0};
        std::atomic<uint64_t> nextCancelIndex{0};
        std::atomic<uint64_t> completedSleeps{0};
        std::atomic<uint64_t> completedCancels{0};
    };

    struct SleepArgs
    {
        SleepContext * ctx;
    };

    struct TimedItem
    {
        FiberFuture target;
        std::atomic<uint64_t> waitStartCycles{0};
        std::atomic<int> result{PENDING};
        uint64_t timeoutNs{0};
        int64_t targetOffsetCycles{0};
        bool signal{false};
    };

    struct TimedContext
    {
        TimedItem * items{nullptr};
        uint64_t itemCount{0};
        uint64_t armWaitBudgetCycles{0};
        std::atomic<uint64_t> nextWaitIndex{0};
        std::atomic<uint64_t> nextSignalIndex{0};
        std::atomic<uint64_t> completedWaits{0};
        std::atomic<uint64_t> completedSignals{0};
    };

    struct TimedArgs
    {
        TimedContext * ctx;
    };

    // Deterministic per-index mixing so a failing run reproduces the same
    // duration/jitter schedule. splitmix64 finalizer.
    static uint64_t mix(uint64_t value) noexcept
    {
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31);
    }

    static uint64_t pickDurationNs(uint64_t hash) noexcept { return DURATION_MIN_NS + hash % (DURATION_MAX_NS - DURATION_MIN_NS); }

    // Offset from the arm point to the cancel/signal instant: the deadline plus a
    // signed jitter, clamped so the offset never goes negative (which would wrap
    // the unsigned target and stall the worker indefinitely).
    static int64_t pickTargetOffsetCycles(uint64_t durationNs, uint64_t jitterHash) noexcept
    {
        int64_t jitterNs = static_cast<int64_t>(jitterHash % (2 * JITTER_NS)) - static_cast<int64_t>(JITTER_NS);
        int64_t offsetCycles = static_cast<int64_t>(Tsc::nanosecondsToCycles(durationNs)) + jitterNs;

        if (offsetCycles < 0)
        {
            offsetCycles = 0;
        }
        return offsetCycles;
    }

    static uint64_t envOverride(const char * name, uint64_t fallback) noexcept
    {
        const char * value = ::getenv(name);
        if (value)
        {
            uint64_t parsed = ::strtoull(value, nullptr, 10);
            if (parsed)
            {
                return parsed;
            }
        }
        return fallback;
    }

    static uint64_t itemCount() noexcept { return envOverride("SILK_SLEEP_STRESS_ITEMS", DEFAULT_ITEM_COUNT); }

    static uint64_t watchdogSeconds() noexcept { return envOverride("SILK_SLEEP_STRESS_WATCHDOG_S", DEFAULT_WATCHDOG_SECONDS); }

    // A stuck sleeper or waiter can never be joined, so a detected lost wakeup
    // cannot fall through to teardown - that would crash on running fibers and
    // bury the diagnostic. Record the failure and exit the (single-test) process.
    [[noreturn]] static void
    failLostWakeup(const char * labelA, uint64_t doneA, const char * labelB, uint64_t doneB, uint64_t total) noexcept
    {
        ADD_FAILURE() << "lost wakeup: " << doneA << "/" << total << " " << labelA << " and " << doneB << "/" << total << " " << labelB
                      << " completed before the watchdog fired";

        std::fflush(nullptr);
        std::_Exit(EXIT_FAILURE);
    }

    //
    // Raw sleep expiry versus cancel.
    //

    static int sleeperMain(SleepArgs * args) noexcept
    {
        SleepContext * ctx = args->ctx;

        for (;;)
        {
            uint64_t index = ctx->nextSleepIndex.fetch_add(1, std::memory_order_relaxed);
            if (index >= ctx->itemCount)
            {
                break;
            }

            SleepItem * item = &ctx->items[index];
            uint64_t armCycles = Tsc::getCycles();

            FiberScheduler::sleep(item->durationNs, &item->future);

            // Publish the arm point only after sleep() has reserved the deadline so
            // the canceller never aligns to a stale instant.
            item->armCycles.store(armCycles, std::memory_order_release);

            int r = item->future.wait();
            item->result.store(r, std::memory_order_relaxed);
            ctx->completedSleeps.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }

    static int fiberCancellerMain(SleepArgs * args) noexcept
    {
        SleepContext * ctx = args->ctx;

        for (;;)
        {
            uint64_t index = ctx->nextCancelIndex.fetch_add(1, std::memory_order_relaxed);
            if (index >= ctx->itemCount)
            {
                break;
            }

            SleepItem * item = &ctx->items[index];

            if (!item->cancel)
            {
                ctx->completedCancels.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            uint64_t armCycles = waitForArmCooperative(&item->armCycles, ctx->armWaitBudgetCycles);
            if (armCycles)
            {
                uint64_t targetCycles = armCycles + static_cast<uint64_t>(item->targetOffsetCycles);
                sleepUntilCooperative(targetCycles);
                item->future.cancel();
            }

            ctx->completedCancels.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }

    // External-thread cancel path: drives cancelSleep from outside the scheduler
    // so the cross-thread wakeThread/parkThread handoff is exercised too.
    static void threadCancellerLoop(SleepContext * ctx) noexcept
    {
        for (;;)
        {
            uint64_t index = ctx->nextCancelIndex.fetch_add(1, std::memory_order_relaxed);
            if (index >= ctx->itemCount)
            {
                break;
            }

            SleepItem * item = &ctx->items[index];

            if (!item->cancel)
            {
                ctx->completedCancels.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            uint64_t armCycles = waitForArmSpinning(&item->armCycles, ctx->armWaitBudgetCycles);
            if (armCycles)
            {
                uint64_t targetCycles = armCycles + static_cast<uint64_t>(item->targetOffsetCycles);
                spinUntil(targetCycles);
                item->future.cancel();
            }

            ctx->completedCancels.fetch_add(1, std::memory_order_relaxed);
        }
    }

    //
    // waitWithTimeout versus signal: the realistic sleep-timeout-plus-cancellation
    // path, where the internal SleepFuture races a concurrent signal of the target.
    //

    static int waiterMain(TimedArgs * args) noexcept
    {
        TimedContext * ctx = args->ctx;

        for (;;)
        {
            uint64_t index = ctx->nextWaitIndex.fetch_add(1, std::memory_order_relaxed);
            if (index >= ctx->itemCount)
            {
                break;
            }

            TimedItem * item = &ctx->items[index];
            item->waitStartCycles.store(Tsc::getCycles(), std::memory_order_release);

            int r = FiberFuture::waitWithTimeout(&item->target, item->timeoutNs);
            item->result.store(r, std::memory_order_relaxed);
            ctx->completedWaits.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }

    // Signal from an external thread, spin-aligned to the deadline. Threads can
    // spin without starving the silk scheduler, so the only way a waiter can stick
    // is a genuine lost wakeup - no fiber busy-yield to confound the result.
    static void threadSignallerLoop(TimedContext * ctx) noexcept
    {
        for (;;)
        {
            uint64_t index = ctx->nextSignalIndex.fetch_add(1, std::memory_order_relaxed);
            if (index >= ctx->itemCount)
            {
                break;
            }

            TimedItem * item = &ctx->items[index];

            if (!item->signal)
            {
                ctx->completedSignals.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            uint64_t startCycles = waitForArmSpinning(&item->waitStartCycles, ctx->armWaitBudgetCycles);
            if (startCycles)
            {
                uint64_t targetCycles = startCycles + static_cast<uint64_t>(item->targetOffsetCycles);
                spinUntil(targetCycles);
                item->target.set(0);
            }

            ctx->completedSignals.fetch_add(1, std::memory_order_relaxed);
        }
    }

    //
    // Shared waiting primitives.
    //

    static uint64_t waitForArmCooperative(std::atomic<uint64_t> * armCycles, uint64_t budgetCycles) noexcept
    {
        uint64_t waitStart = Tsc::getCycles();
        for (;;)
        {
            uint64_t value = armCycles->load(std::memory_order_acquire);
            if (value)
            {
                return value;
            }

            uint64_t elapsed = Tsc::getCycles() - waitStart;
            if (elapsed > budgetCycles)
            {
                return 0;
            }

            // A yield storm here starves runServiceLoop unless readyDispatchBatch
            // bounds handleReadyQueue - this loop is the regression for that fix.
            FiberScheduler::yield();
        }
    }

    static uint64_t waitForArmSpinning(std::atomic<uint64_t> * armCycles, uint64_t budgetCycles) noexcept
    {
        uint64_t waitStart = Tsc::getCycles();
        for (;;)
        {
            uint64_t value = armCycles->load(std::memory_order_acquire);
            if (value)
            {
                return value;
            }

            uint64_t elapsed = Tsc::getCycles() - waitStart;
            if (elapsed > budgetCycles)
            {
                return 0;
            }

            std::this_thread::yield();
        }
    }

    static void sleepUntilCooperative(uint64_t targetCycles) noexcept
    {
        uint64_t now = Tsc::getCycles();
        if (now >= targetCycles)
        {
            return;
        }

        uint64_t remainingNs = Tsc::cyclesToNanoseconds(targetCycles - now);
        FiberScheduler::sleep(remainingNs);
    }

    static void spinUntil(uint64_t targetCycles) noexcept
    {
        for (;;)
        {
            uint64_t now = Tsc::getCycles();
            if (now >= targetCycles)
            {
                return;
            }

            std::this_thread::yield();
        }
    }

    //
    // Main-thread watchdog and result checks.
    //

    static bool waitForCompletion(std::atomic<uint64_t> * counterA, std::atomic<uint64_t> * counterB, uint64_t target) noexcept
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(watchdogSeconds());
        for (;;)
        {
            uint64_t a = counterA->load(std::memory_order_relaxed);
            uint64_t b = counterB->load(std::memory_order_relaxed);
            if (a >= target && b >= target)
            {
                return true;
            }

            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
};

// Raw sleep expiry racing a cancel issued from another fiber (and, via work
// stealing, very likely another CPU than the sleep's owning processor).
TEST(SleepCancel, expiryVsCancelFibers)
{
    const uint64_t count = SleepStress::itemCount();
    std::unique_ptr<SleepStress::SleepItem[]> items(new SleepStress::SleepItem[count]);

    for (uint64_t i = 0; i < count; ++i)
    {
        uint64_t durationNs = SleepStress::pickDurationNs(SleepStress::mix(2 * i));
        items[i].durationNs = durationNs;
        items[i].targetOffsetCycles = SleepStress::pickTargetOffsetCycles(durationNs, SleepStress::mix(2 * i + 1));
        items[i].cancel = SleepStress::mix(i ^ 0xABCDEF) % 100 < SleepStress::CANCEL_PERCENT;
    }

    SleepStress::SleepContext ctx;
    ctx.items = items.get();
    ctx.itemCount = count;
    ctx.armWaitBudgetCycles = Tsc::nanosecondsToCycles(SleepStress::ARM_WAIT_BUDGET_NS);

    FiberFuture sleeperFutures[SleepStress::NUM_SLEEPERS];
    FiberFuture cancellerFutures[SleepStress::NUM_CANCELLERS];

    for (uint32_t i = 0; i < SleepStress::NUM_SLEEPERS; ++i)
    {
        int r = FiberScheduler::run(SleepStress::sleeperMain, {&ctx}, &sleeperFutures[i]);
        ASSERT_FALSE(r);
    }

    for (uint32_t i = 0; i < SleepStress::NUM_CANCELLERS; ++i)
    {
        int r = FiberScheduler::run(SleepStress::fiberCancellerMain, {&ctx}, &cancellerFutures[i]);
        ASSERT_FALSE(r);
    }

    bool completed = SleepStress::waitForCompletion(&ctx.completedSleeps, &ctx.completedCancels, count);

    if (!completed)
    {
        uint64_t doneSleeps = ctx.completedSleeps.load(std::memory_order_relaxed);
        uint64_t doneCancels = ctx.completedCancels.load(std::memory_order_relaxed);
        SleepStress::failLostWakeup("sleeps", doneSleeps, "cancels", doneCancels, count);
    }

    for (uint32_t i = 0; i < SleepStress::NUM_SLEEPERS; ++i)
    {
        sleeperFutures[i].wait();
    }

    for (uint32_t i = 0; i < SleepStress::NUM_CANCELLERS; ++i)
    {
        cancellerFutures[i].wait();
    }

    for (uint64_t i = 0; i < count; ++i)
    {
        int r = items[i].result.load(std::memory_order_relaxed);
        if (items[i].cancel)
        {
            bool valid = r == 0 || r == ECANCELED;
            EXPECT_TRUE(valid) << "item " << i << " resolved with unexpected result " << r;
        }
        else
        {
            EXPECT_EQ(r, 0) << "uncancelled item " << i << " did not expire cleanly";
        }
    }
}

// Same race, but cancellation is driven from external threads so the cross-thread
// cancelSleep -> cancelQueue -> wakeThread path (and the park-side seq_cst pairing)
// is under load too.
TEST(SleepCancel, expiryVsCancelThreads)
{
    const uint64_t count = SleepStress::itemCount();
    std::unique_ptr<SleepStress::SleepItem[]> items(new SleepStress::SleepItem[count]);

    for (uint64_t i = 0; i < count; ++i)
    {
        uint64_t durationNs = SleepStress::pickDurationNs(SleepStress::mix(2 * i));
        items[i].durationNs = durationNs;
        items[i].targetOffsetCycles = SleepStress::pickTargetOffsetCycles(durationNs, SleepStress::mix(2 * i + 1));
        items[i].cancel = SleepStress::mix(i ^ 0xABCDEF) % 100 < SleepStress::CANCEL_PERCENT;
    }

    SleepStress::SleepContext ctx;
    ctx.items = items.get();
    ctx.itemCount = count;
    ctx.armWaitBudgetCycles = Tsc::nanosecondsToCycles(SleepStress::ARM_WAIT_BUDGET_NS);

    FiberFuture sleeperFutures[SleepStress::NUM_SLEEPERS];

    for (uint32_t i = 0; i < SleepStress::NUM_SLEEPERS; ++i)
    {
        int r = FiberScheduler::run(SleepStress::sleeperMain, {&ctx}, &sleeperFutures[i]);
        ASSERT_FALSE(r);
    }

    std::vector<std::thread> cancellers;
    cancellers.reserve(SleepStress::NUM_CANCEL_THREADS);

    for (uint32_t i = 0; i < SleepStress::NUM_CANCEL_THREADS; ++i)
    {
        cancellers.emplace_back(SleepStress::threadCancellerLoop, &ctx);
    }

    bool completed = SleepStress::waitForCompletion(&ctx.completedSleeps, &ctx.completedCancels, count);

    for (std::thread & canceller : cancellers)
    {
        canceller.join();
    }

    if (!completed)
    {
        uint64_t doneSleeps = ctx.completedSleeps.load(std::memory_order_relaxed);
        uint64_t doneCancels = ctx.completedCancels.load(std::memory_order_relaxed);
        SleepStress::failLostWakeup("sleeps", doneSleeps, "cancels", doneCancels, count);
    }

    for (uint32_t i = 0; i < SleepStress::NUM_SLEEPERS; ++i)
    {
        sleeperFutures[i].wait();
    }

    for (uint64_t i = 0; i < count; ++i)
    {
        int r = items[i].result.load(std::memory_order_relaxed);
        if (items[i].cancel)
        {
            bool valid = r == 0 || r == ECANCELED;
            EXPECT_TRUE(valid) << "item " << i << " resolved with unexpected result " << r;
        }
        else
        {
            EXPECT_EQ(r, 0) << "uncancelled item " << i << " did not expire cleanly";
        }
    }
}

// waitWithTimeout racing a signal that fires at the timeout deadline plus jitter:
// the target future and the internal sleep complete near-simultaneously, stressing
// waitForMultiple plus the internal sleep cancellation.
TEST(SleepCancel, waitWithTimeoutVsSignal)
{
    const uint64_t count = SleepStress::itemCount();
    std::unique_ptr<SleepStress::TimedItem[]> items(new SleepStress::TimedItem[count]);

    for (uint64_t i = 0; i < count; ++i)
    {
        uint64_t timeoutNs = SleepStress::pickDurationNs(SleepStress::mix(2 * i));
        items[i].timeoutNs = timeoutNs;
        items[i].targetOffsetCycles = SleepStress::pickTargetOffsetCycles(timeoutNs, SleepStress::mix(2 * i + 1));
        items[i].signal = SleepStress::mix(i ^ 0xABCDEF) % 100 < SleepStress::CANCEL_PERCENT;
    }

    SleepStress::TimedContext ctx;
    ctx.items = items.get();
    ctx.itemCount = count;
    ctx.armWaitBudgetCycles = Tsc::nanosecondsToCycles(SleepStress::ARM_WAIT_BUDGET_NS);

    FiberFuture waiterFutures[SleepStress::NUM_WAITERS];

    for (uint32_t i = 0; i < SleepStress::NUM_WAITERS; ++i)
    {
        int r = FiberScheduler::run(SleepStress::waiterMain, {&ctx}, &waiterFutures[i]);
        ASSERT_FALSE(r);
    }

    std::vector<std::thread> signallers;
    signallers.reserve(SleepStress::NUM_CANCEL_THREADS);

    for (uint32_t i = 0; i < SleepStress::NUM_CANCEL_THREADS; ++i)
    {
        signallers.emplace_back(SleepStress::threadSignallerLoop, &ctx);
    }

    bool completed = SleepStress::waitForCompletion(&ctx.completedWaits, &ctx.completedSignals, count);

    for (std::thread & signaller : signallers)
    {
        signaller.join();
    }

    if (!completed)
    {
        uint64_t doneWaits = ctx.completedWaits.load(std::memory_order_relaxed);
        uint64_t doneSignals = ctx.completedSignals.load(std::memory_order_relaxed);
        SleepStress::failLostWakeup("waits", doneWaits, "signals", doneSignals, count);
    }

    for (uint32_t i = 0; i < SleepStress::NUM_WAITERS; ++i)
    {
        waiterFutures[i].wait();
    }

    for (uint64_t i = 0; i < count; ++i)
    {
        int r = items[i].result.load(std::memory_order_relaxed);
        if (items[i].signal)
        {
            bool valid = r == 0 || r == ETIMEDOUT;
            EXPECT_TRUE(valid) << "item " << i << " resolved with unexpected result " << r;
        }
        else
        {
            EXPECT_EQ(r, ETIMEDOUT) << "unsignalled item " << i << " did not time out";
        }
    }
}

} // namespace silk
