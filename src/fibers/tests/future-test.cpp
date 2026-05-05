#include <silk/fibers/future.h>

#include <silk/fibers/fiber.h>

#include <gtest/gtest.h>

#include <cerrno>

namespace silk
{

TEST(FiberFuture, waitSignaled)
{
    FiberFuture future;

    int r;
    EXPECT_FALSE(future.isSet(&r));

    future.set(42);

    EXPECT_TRUE(future.isSet(&r));
    EXPECT_EQ(r, 42);

    r = future.wait();
    EXPECT_EQ(r, 42);

    future.reset();
    EXPECT_FALSE(future.isSet(&r));
}

TEST(FiberFuture, waitFromFiber)
{
    struct Params
    {
        FiberFuture * started;
        FiberFuture * future;

        static int fiberMain(Params * p) noexcept
        {
            p->started->set(0);
            return p->future->wait();
        }
    };

    FiberFuture started, waiting, future;
    int r = FiberScheduler::run(Params::fiberMain, {&started, &waiting}, &future);
    ASSERT_FALSE(r);

    started.wait();
    waiting.set(42);

    r = future.wait();
    ASSERT_EQ(r, 42);
}

TEST(FiberFuture, waitFromThread)
{
    struct Params
    {
        FiberFuture * future;

        static int fiberMain(Params * p) noexcept
        {
            p->future->set(42);
            return 0;
        }
    };

    FiberFuture future, completion;
    int r = FiberScheduler::run(Params::fiberMain, {&future}, &completion);
    ASSERT_FALSE(r);

    r = future.wait();
    ASSERT_EQ(r, 42);

    completion.wait();
}

// waitForMultiple with a single future uses the N=1 fast path and returns 0.
TEST(FiberFuture, waitForMultipleSingle)
{
    struct Params
    {
        FiberFuture * future;

        static int fiberMain(Params * p) noexcept
        {
            p->future->set(0);
            return 0;
        }
    };

    FiberFuture future, completion;
    int r = FiberScheduler::run(Params::fiberMain, {&future}, &completion);
    ASSERT_FALSE(r);

    FiberFuture * ptrs[] = {&future};
    uint64_t index = FiberFuture::waitForMultiple(ptrs, 1);
    EXPECT_EQ(index, 0u);

    completion.wait();
}

// waitForMultiple with an empty array returns immediately.
TEST(FiberFuture, waitForMultipleEmpty)
{
    FiberFuture::waitForMultiple(nullptr, 0);
}

// waitForMultiple returns immediately when the first future is already set.
TEST(FiberFuture, waitForMultipleFirstAlreadySet)
{
    static constexpr int N = 4;
    FiberFuture futures[N];
    FiberFuture * ptrs[] = {&futures[0], &futures[1], &futures[2], &futures[3]};

    futures[0].set(0);
    FiberFuture::waitForMultiple(ptrs, N);
}

// waitForMultiple returns immediately when a middle future is already set,
// and correctly detaches from futures that were already attached.
TEST(FiberFuture, waitForMultipleMiddleAlreadySet)
{
    static constexpr int N = 4;
    FiberFuture futures[N];
    FiberFuture * ptrs[] = {&futures[0], &futures[1], &futures[2], &futures[3]};

    futures[2].set(0);
    FiberFuture::waitForMultiple(ptrs, N);
}

// Fiber calls waitForMultiple; the thread signals one of the futures.
TEST(FiberFuture, waitForMultipleFromFiber)
{
    static constexpr int N = 4;

    struct Params
    {
        FiberFuture ** futures;
        FiberFuture * started;

        static int fiberMain(Params * p) noexcept
        {
            p->started->set(0);
            FiberFuture::waitForMultiple(p->futures, N);
            return 0;
        }
    };

    FiberFuture futures[N];
    FiberFuture * ptrs[] = {&futures[0], &futures[1], &futures[2], &futures[3]};
    FiberFuture started, future;
    int r = FiberScheduler::run(Params::fiberMain, {ptrs, &started}, &future);
    ASSERT_FALSE(r);

    started.wait();
    futures[1].set(0);

    future.wait();
}

// Thread calls waitForMultiple; a fiber signals one of the futures.
TEST(FiberFuture, waitForMultipleFromThread)
{
    static constexpr int N = 4;

    struct Params
    {
        FiberFuture * futures;
        int index;

        static int fiberMain(Params * p) noexcept
        {
            p->futures[p->index].set(0);
            return 0;
        }
    };

    FiberFuture futures[N];
    FiberFuture * ptrs[] = {&futures[0], &futures[1], &futures[2], &futures[3]};
    FiberFuture future;
    int r = FiberScheduler::run(Params::fiberMain, {futures, 2}, &future);
    ASSERT_FALSE(r);

    FiberFuture::waitForMultiple(ptrs, N);

    future.wait();
}

// Stress: N fibers each signal a different future; waitForMultiple must wake
// on the first completion and safely clean up the rest.
TEST(FiberFuture, waitForMultipleConcurrent)
{
    static constexpr int N = 16;
    static constexpr int ITERATIONS = 1000;

    struct Params
    {
        FiberFuture * future;

        static int fiberMain(Params * p) noexcept
        {
            p->future->set(0);
            return 0;
        }
    };

    for (int iter = 0; iter < ITERATIONS; ++iter)
    {
        FiberFuture futures[N];
        FiberFuture * ptrs[N];
        FiberFuture completions[N];

        for (int i = 0; i < N; ++i)
        {
            ptrs[i] = &futures[i];
            int r = FiberScheduler::run(Params::fiberMain, {&futures[i]}, &completions[i]);
            ASSERT_FALSE(r);
        }

        FiberFuture::waitForMultiple(ptrs, N);

        for (int i = 0; i < N; ++i)
        {
            completions[i].wait();
        }
    }
}

// wait-with-timeout: zero nanoseconds -> expires immediately without suspending.
TEST(FiberFuture, waitWithTimeoutZero)
{
    FiberFuture future;
    int r = FiberFuture::waitWithTimeout(&future, 0);
    EXPECT_EQ(r, ETIMEDOUT);
}

// wait-with-timeout: future is set before the deadline -> returns the error value.
TEST(FiberFuture, waitWithTimeoutCompletes)
{
    static constexpr uint64_t WAIT_NS = 1'000'000'000; // 1s

    struct Params
    {
        FiberFuture * future;

        static int fiberMain(Params * p) noexcept
        {
            p->future->set(42);
            return 0;
        }
    };

    FiberFuture future, completion;
    int r = FiberScheduler::run(Params::fiberMain, {&future}, &completion);
    ASSERT_FALSE(r);

    r = FiberFuture::waitWithTimeout(&future, WAIT_NS);
    EXPECT_EQ(r, 42);

    completion.wait();
}

// wait-with-timeout: deadline expires before the future is set -> returns ETIMEDOUT.
TEST(FiberFuture, waitWithTimeoutExpires)
{
    static constexpr uint64_t WAIT_NS = 1'000'000; // 1ms

    FiberFuture future;

    int r = FiberFuture::waitWithTimeout(&future, WAIT_NS);
    EXPECT_EQ(r, ETIMEDOUT);
}

// subscribe() returns false when the future is already set; the callback is not invoked.
TEST(FiberFuture, subscribeAlreadySet)
{
    struct State
    {
        FiberFuture future;
        bool called = false;

        static void callback(FiberFuture * future) noexcept { reinterpret_cast<State *>(future)->called = true; }
    };

    State state;
    state.future.set(42);

    bool registered = state.future.subscribe(State::callback);
    EXPECT_FALSE(registered);
    EXPECT_FALSE(state.called);
}

// subscribe() returns true when the future is not yet set; the callback is
// invoked by signal() on completion with the correct error.
TEST(FiberFuture, subscribeBeforeSet)
{
    struct State
    {
        FiberFuture future;
        bool called = false;
        int error = -1;

        static void callback(FiberFuture * future) noexcept
        {
            State * state = reinterpret_cast<State *>(future);
            state->error = future->wait();
            state->called = true;
        }
    };

    State state;
    bool registered = state.future.subscribe(State::callback);
    ASSERT_TRUE(registered);

    state.future.set(42);

    EXPECT_TRUE(state.called);
    EXPECT_EQ(state.error, 42);
}

// waitForMultiple returns the index of the future that is set.
TEST(FiberFuture, waitForMultipleReturnIndex)
{
    static constexpr int N = 4;

    struct Params
    {
        FiberFuture * futures;
        int index;

        static int fiberMain(Params * p) noexcept
        {
            p->futures[p->index].set(0);
            return 0;
        }
    };

    for (int target = 0; target < N; ++target)
    {
        FiberFuture futures[N];
        FiberFuture * ptrs[N] = {&futures[0], &futures[1], &futures[2], &futures[3]};
        FiberFuture completion;
        int r = FiberScheduler::run(Params::fiberMain, {futures, target}, &completion);
        ASSERT_FALSE(r);

        uint64_t index = FiberFuture::waitForMultiple(ptrs, N);
        EXPECT_EQ(static_cast<int>(index), target);

        completion.wait();
        // Drain remaining futures so none are left with waiters.
        for (int i = 0; i < N; ++i)
        {
            int err;
            if (!futures[i].isSet(&err))
            {
                futures[i].set(0);
            }
        }
    }
}

// reset() clears the future so it can be reused for the next set/wait cycle.
TEST(FiberFuture, resetAndReuse)
{
    FiberFuture future;

    future.set(1);
    ASSERT_EQ(future.wait(), 1);

    future.reset();
    int r;
    ASSERT_FALSE(future.isSet(&r));

    future.set(2);
    ASSERT_EQ(future.wait(), 2);
}

} // namespace silk
