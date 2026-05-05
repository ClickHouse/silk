#include <silk/fibers/event.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>

#include <gtest/gtest.h>

namespace silk
{

TEST(FiberEvent, initiallyUnset)
{
    FiberEvent event;
    ASSERT_FALSE(event.isSet());
}

TEST(FiberEvent, isSetAfterSet)
{
    FiberEvent event;
    event.set();
    ASSERT_TRUE(event.isSet());
}

TEST(FiberEvent, isUnsetAfterReset)
{
    FiberEvent event;
    event.set();
    event.reset();
    ASSERT_FALSE(event.isSet());
}

TEST(FiberEvent, setIdempotent)
{
    FiberEvent event;
    event.set();
    event.set();
    ASSERT_TRUE(event.isSet());
}

TEST(FiberEvent, resetIdempotent)
{
    FiberEvent event;
    event.reset();
    event.reset();
    ASSERT_FALSE(event.isSet());
}

TEST(FiberEvent, waitAlreadySet)
{
    FiberEvent event;
    event.set();
    event.wait();
}

TEST(FiberEvent, asyncWaitAlreadySet)
{
    FiberEvent event;
    event.set();
    FiberEvent::Future future;
    event.wait(&future);
    int err;
    EXPECT_TRUE(future.isSet(&err));
    EXPECT_EQ(err, 0);
}

// Blocking wait() (not async wait(Future*)) suspends the fiber until set.
TEST(FiberEvent, waitBlockingSuspends)
{
    struct Params
    {
        FiberEvent * event;
        FiberFuture * waiting;

        static int fiberMain(Params * p) noexcept
        {
            p->waiting->set(0);
            p->event->wait(); // blocking form: suspends until set()
            return 0;
        }
    };

    FiberEvent event;
    FiberFuture future, waiting;
    int r = FiberScheduler::run(Params::fiberMain, {&event, &waiting}, &future);
    ASSERT_FALSE(r);

    waiting.wait();
    event.set();
    future.wait();
}

TEST(FiberEvent, waitSuspends)
{
    struct Params
    {
        FiberEvent * event;
        FiberEvent::Future * future;
        FiberFuture * registered;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            p->event->wait(p->future);
            p->registered->set(0);
            p->future->wait();
            p->done->set(0);
            return 0;
        }
    };

    FiberEvent event;
    FiberEvent::Future eventFuture;
    FiberFuture future, registered, done;
    int r = FiberScheduler::run(Params::fiberMain, {&event, &eventFuture, &registered, &done}, &future);
    ASSERT_FALSE(r);

    registered.wait();
    event.set();
    done.wait();
    future.wait();
}

TEST(FiberEvent, setWakesAll)
{
    static constexpr int N = 4;

    struct Params
    {
        FiberEvent * event;
        FiberEvent::Future * future;
        FiberFuture * registered;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            p->event->wait(p->future);
            p->registered->set(0);
            p->future->wait();
            p->done->set(0);
            return 0;
        }
    };

    FiberEvent event;
    FiberEvent::Future eventFutures[N];
    FiberFuture futures[N], registered[N], done[N];

    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Params::fiberMain, {&event, &eventFutures[i], &registered[i], &done[i]}, &futures[i]);
        ASSERT_FALSE(r);
        registered[i].wait();
    }

    event.set();

    for (int i = 0; i < N; ++i)
    {
        done[i].wait();
        futures[i].wait();
    }
}

TEST(FiberEvent, setResetCycle)
{
    struct Params
    {
        FiberEvent * event;
        FiberEvent::Future * future;
        FiberFuture * registered;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            p->event->wait(p->future);
            p->registered->set(0);
            p->future->wait();
            p->done->set(0);
            return 0;
        }
    };

    FiberEvent event;

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        FiberEvent::Future eventFuture;
        FiberFuture future, registered, done;
        int r = FiberScheduler::run(Params::fiberMain, {&event, &eventFuture, &registered, &done}, &future);
        ASSERT_FALSE(r);

        registered.wait();
        event.set();
        done.wait();
        future.wait();

        event.reset();
        ASSERT_FALSE(event.isSet());
    }
}

TEST(FiberEvent, cancel)
{
    struct Params
    {
        FiberEvent * event;
        FiberEvent::Future * future;
        FiberFuture * registered;
        FiberFuture * done;

        static int fiberMain(Params * p) noexcept
        {
            p->event->wait(p->future);
            p->registered->set(0);
            p->future->wait();
            int err;
            EXPECT_TRUE(p->future->isSet(&err));
            EXPECT_EQ(err, ECANCELED);
            p->done->set(0);
            return 0;
        }
    };

    FiberEvent event;
    FiberEvent::Future eventFuture;
    FiberFuture future, registered, done;
    int r = FiberScheduler::run(Params::fiberMain, {&event, &eventFuture, &registered, &done}, &future);
    ASSERT_FALSE(r);

    registered.wait();
    eventFuture.cancel();
    done.wait();
    future.wait();
}

} // namespace silk
