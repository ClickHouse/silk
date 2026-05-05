#include <silk/fibers/blocking-queue.h>

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>

// Basic SPSC: enqueue then dequeue without blocking (capacity never reached).

namespace silk
{

TEST(FiberBlockingQueue, basicEnqueueDequeue)
{
    FiberBlockingQueue<int> queue(4);
    EXPECT_EQ(queue.enqueue(1), 0);
    EXPECT_EQ(queue.enqueue(2), 0);
    int value = 0;
    EXPECT_EQ(queue.dequeue(&value), 0);
    EXPECT_EQ(value, 1);
    EXPECT_EQ(queue.dequeue(&value), 0);
    EXPECT_EQ(value, 2);
}

// tryEnqueue/tryDequeue: non-blocking; return false when full or empty.
TEST(FiberBlockingQueue, tryEnqueueDequeue)
{
    FiberBlockingQueue<int> queue(2);
    EXPECT_TRUE(queue.tryEnqueue(10));
    EXPECT_TRUE(queue.tryEnqueue(20));
    EXPECT_FALSE(queue.tryEnqueue(30)); // full

    int value = 0;
    EXPECT_TRUE(queue.tryDequeue(&value));
    EXPECT_EQ(value, 10);
    EXPECT_TRUE(queue.tryDequeue(&value));
    EXPECT_EQ(value, 20);
    EXPECT_FALSE(queue.tryDequeue(&value)); // empty
}

// Blocking dequeue: consumer suspends on empty queue; producer enqueues and wakes it.
TEST(FiberBlockingQueue, blockingDequeue)
{
    struct Consumer
    {
        FiberBlockingQueue<int> * queue;
        FiberFuture * started;

        static int fiberMain(Consumer * p) noexcept
        {
            p->started->set(0);
            int value = 0;
            EXPECT_EQ(p->queue->dequeue(&value), 0);
            return value;
        }
    };

    struct Producer
    {
        FiberBlockingQueue<int> * queue;
        FiberFuture * consumerStarted;

        static int fiberMain(Producer * p) noexcept
        {
            p->consumerStarted->wait();
            FiberScheduler::yield();
            EXPECT_EQ(p->queue->enqueue(42), 0);
            return 0;
        }
    };

    FiberBlockingQueue<int> queue(4);
    FiberFuture started;
    FiberFuture consumer, producer;
    int r = FiberScheduler::run(Consumer::fiberMain, {&queue, &started}, &consumer);
    ASSERT_FALSE(r);
    r = FiberScheduler::run(Producer::fiberMain, {&queue, &started}, &producer);
    ASSERT_FALSE(r);

    producer.wait();
    r = consumer.wait();
    ASSERT_EQ(r, 42);
}

// Blocking enqueue: producer suspends on a full queue; consumer dequeues and wakes it.
TEST(FiberBlockingQueue, blockingEnqueue)
{
    struct Producer
    {
        FiberBlockingQueue<int> * queue;
        FiberFuture * blocked;

        static int fiberMain(Producer * p) noexcept
        {
            EXPECT_EQ(p->queue->enqueue(1), 0); // fills capacity-2 queue
            EXPECT_EQ(p->queue->enqueue(2), 0);
            p->blocked->set(0);
            EXPECT_EQ(p->queue->enqueue(3), 0); // must block until consumer dequeues
            return 0;
        }
    };

    struct Consumer
    {
        FiberBlockingQueue<int> * queue;
        FiberFuture * producerBlocked;

        static int fiberMain(Consumer * p) noexcept
        {
            p->producerBlocked->wait();
            FiberScheduler::yield();
            int value = 0;
            EXPECT_EQ(p->queue->dequeue(&value), 0);
            EXPECT_EQ(value, 1);
            return 0;
        }
    };

    FiberBlockingQueue<int> queue(2);
    FiberFuture blocked;
    FiberFuture producer, consumer;
    int r = FiberScheduler::run(Producer::fiberMain, {&queue, &blocked}, &producer);
    ASSERT_FALSE(r);
    r = FiberScheduler::run(Consumer::fiberMain, {&queue, &blocked}, &consumer);
    ASSERT_FALSE(r);

    producer.wait();
    consumer.wait();
}

// teardown wakes a fiber blocked in dequeue with ECANCELED.
TEST(FiberBlockingQueue, teardownUnblocksDequeue)
{
    struct Consumer
    {
        FiberBlockingQueue<int> * queue;
        FiberFuture * started;

        static int fiberMain(Consumer * p) noexcept
        {
            p->started->set(0);
            int value = 0;
            int r = p->queue->dequeue(&value);
            EXPECT_EQ(r, ECANCELED);
            return r;
        }
    };

    struct Teardown
    {
        FiberBlockingQueue<int> * queue;
        FiberFuture * consumerStarted;

        static int fiberMain(Teardown * p) noexcept
        {
            p->consumerStarted->wait();
            FiberScheduler::yield();
            p->queue->teardown();
            return 0;
        }
    };

    FiberBlockingQueue<int> queue(4);
    FiberFuture started;
    FiberFuture consumer, teardown;
    int r = FiberScheduler::run(Consumer::fiberMain, {&queue, &started}, &consumer);
    ASSERT_FALSE(r);
    r = FiberScheduler::run(Teardown::fiberMain, {&queue, &started}, &teardown);
    ASSERT_FALSE(r);

    teardown.wait();
    r = consumer.wait();
    ASSERT_EQ(r, ECANCELED);
}

// teardown wakes a fiber blocked in enqueue with ECANCELED.
TEST(FiberBlockingQueue, teardownUnblocksEnqueue)
{
    struct Producer
    {
        FiberBlockingQueue<int> * queue;
        FiberFuture * blocked;

        static int fiberMain(Producer * p) noexcept
        {
            EXPECT_EQ(p->queue->enqueue(1), 0); // fills capacity-2 queue
            EXPECT_EQ(p->queue->enqueue(2), 0);
            p->blocked->set(0);
            int r = p->queue->enqueue(3); // blocks
            EXPECT_EQ(r, ECANCELED);
            return r;
        }
    };

    struct Teardown
    {
        FiberBlockingQueue<int> * queue;
        FiberFuture * producerBlocked;

        static int fiberMain(Teardown * p) noexcept
        {
            p->producerBlocked->wait();
            FiberScheduler::yield();
            p->queue->teardown();
            return 0;
        }
    };

    FiberBlockingQueue<int> queue(2);
    FiberFuture blocked;
    FiberFuture producer, teardown;
    int r = FiberScheduler::run(Producer::fiberMain, {&queue, &blocked}, &producer);
    ASSERT_FALSE(r);
    r = FiberScheduler::run(Teardown::fiberMain, {&queue, &blocked}, &teardown);
    ASSERT_FALSE(r);

    teardown.wait();
    r = producer.wait();
    ASSERT_EQ(r, ECANCELED);
}

// After teardown, blocking enqueue and dequeue return ECANCELED immediately.
TEST(FiberBlockingQueue, teardownSubsequentCallsFail)
{
    FiberBlockingQueue<int> queue(4);
    queue.teardown();
    EXPECT_EQ(queue.enqueue(1), ECANCELED);
    int value = 0;
    EXPECT_EQ(queue.dequeue(&value), ECANCELED);
}

// After teardown, tryEnqueue and tryDequeue return false immediately.
TEST(FiberBlockingQueue, teardownTryCallsFail)
{
    FiberBlockingQueue<int> queue(4);
    queue.teardown();
    EXPECT_FALSE(queue.tryEnqueue(1));
    int value = 0;
    EXPECT_FALSE(queue.tryDequeue(&value));
}

// teardown wakes all fibers blocked in dequeue simultaneously.
TEST(FiberBlockingQueue, teardownUnblocksMultipleWaiters)
{
    static constexpr int N = 8;

    struct Consumer
    {
        FiberBlockingQueue<int> * queue;
        FiberFuture * started;

        static int fiberMain(Consumer * p) noexcept
        {
            p->started->set(0);
            int value = 0;
            int r = p->queue->dequeue(&value);
            EXPECT_EQ(r, ECANCELED);
            return r;
        }
    };

    struct Teardown
    {
        FiberBlockingQueue<int> * queue;
        FiberFuture * allStarted;

        static int fiberMain(Teardown * p) noexcept
        {
            p->allStarted->wait();
            FiberScheduler::yield();
            p->queue->teardown();
            return 0;
        }
    };

    FiberBlockingQueue<int> queue(4);
    FiberFuture started[N], consumers[N];
    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Consumer::fiberMain, {&queue, &started[i]}, &consumers[i]);
        ASSERT_FALSE(r);
    }

    // Wait for all consumers to start, then trigger teardown from a fiber.
    FiberFuture allStarted, teardown;
    struct Barrier
    {
        FiberFuture * started;
        int n;
        FiberFuture * done;

        static int fiberMain(Barrier * p) noexcept
        {
            for (int i = 0; i < p->n; ++i)
            {
                p->started[i].wait();
            }
            p->done->set(0);
            return 0;
        }
    };
    int r = FiberScheduler::run(Barrier::fiberMain, {started, N, &allStarted}, &teardown);
    ASSERT_FALSE(r);
    teardown.wait();

    FiberFuture teardownFiber;
    r = FiberScheduler::run(Teardown::fiberMain, {&queue, &allStarted}, &teardownFiber);
    ASSERT_FALSE(r);
    teardownFiber.wait();

    for (int i = 0; i < N; ++i)
    {
        int r = consumers[i].wait();
        EXPECT_EQ(r, ECANCELED);
    }
}

// FIFO ordering: items must come out in the order they were enqueued.
TEST(FiberBlockingQueue, fifoOrdering)
{
    static constexpr int N = 64;

    struct Producer
    {
        FiberBlockingQueue<int> * queue;

        static int fiberMain(Producer * p) noexcept
        {
            for (int i = 0; i < N; ++i)
            {
                EXPECT_EQ(p->queue->enqueue(i), 0);
            }
            return 0;
        }
    };

    struct Consumer
    {
        FiberBlockingQueue<int> * queue;

        static int fiberMain(Consumer * p) noexcept
        {
            for (int i = 0; i < N; ++i)
            {
                int value = 0;
                EXPECT_EQ(p->queue->dequeue(&value), 0);
                EXPECT_EQ(value, i);
            }
            return 0;
        }
    };

    FiberBlockingQueue<int> queue(4);
    FiberFuture producer, consumer;
    int r = FiberScheduler::run(Producer::fiberMain, {&queue}, &producer);
    ASSERT_FALSE(r);
    r = FiberScheduler::run(Consumer::fiberMain, {&queue}, &consumer);
    ASSERT_FALSE(r);

    producer.wait();
    consumer.wait();
}

// MPMC stress: N producers and N consumers; total items transferred must match.
TEST(FiberBlockingQueue, concurrentMPMC)
{
    static constexpr int N = 8;
    static constexpr int ITER = 200;

    struct Producer
    {
        FiberBlockingQueue<int> * queue;

        static int fiberMain(Producer * p) noexcept
        {
            for (int i = 0; i < ITER; ++i)
            {
                EXPECT_EQ(p->queue->enqueue(1), 0);
            }
            return 0;
        }
    };

    struct Consumer
    {
        FiberBlockingQueue<int> * queue;
        std::atomic<int> * total;

        static int fiberMain(Consumer * p) noexcept
        {
            for (int i = 0; i < ITER; ++i)
            {
                int value = 0;
                EXPECT_EQ(p->queue->dequeue(&value), 0);
                p->total->fetch_add(value, std::memory_order_relaxed);
            }
            return 0;
        }
    };

    FiberBlockingQueue<int> queue(N);
    std::atomic<int> total{};
    FiberFuture producers[N], consumers[N];

    for (int i = 0; i < N; ++i)
    {
        int r = FiberScheduler::run(Producer::fiberMain, {&queue}, &producers[i]);
        ASSERT_FALSE(r);
        r = FiberScheduler::run(Consumer::fiberMain, {&queue, &total}, &consumers[i]);
        ASSERT_FALSE(r);
    }
    for (int i = 0; i < N; ++i)
    {
        producers[i].wait();
        consumers[i].wait();
    }

    ASSERT_EQ(total.load(), N * ITER);
}

} // namespace silk
