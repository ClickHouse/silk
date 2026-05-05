#include <silk/util/bounded-queue.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace silk
{

using MyQueue = BoundedQueue<int>;

static constexpr uint64_t CAPACITY = 64;

TEST(BoundedQueue, DequeueEmptyReturnsFalse)
{
    MyQueue queue{CAPACITY};
    int value;
    EXPECT_FALSE(queue.dequeue(&value));
}

TEST(BoundedQueue, EmptyReturnsTrue)
{
    MyQueue queue{CAPACITY};
    EXPECT_TRUE(queue.empty());
}

TEST(BoundedQueue, EnqueueDequeue)
{
    MyQueue queue{CAPACITY};

    EXPECT_TRUE(queue.enqueue(42));
    EXPECT_FALSE(queue.empty());

    int result;
    EXPECT_TRUE(queue.dequeue(&result));
    EXPECT_EQ(result, 42);

    int discard;
    EXPECT_FALSE(queue.dequeue(&discard));
    EXPECT_TRUE(queue.empty());
}

TEST(BoundedQueue, FIFOOrder)
{
    MyQueue queue{CAPACITY};

    EXPECT_TRUE(queue.enqueue(1));
    EXPECT_TRUE(queue.enqueue(2));
    EXPECT_TRUE(queue.enqueue(3));

    int value;
    EXPECT_TRUE(queue.dequeue(&value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(queue.dequeue(&value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(queue.dequeue(&value));
    EXPECT_EQ(value, 3);

    EXPECT_FALSE(queue.dequeue(&value));
}

TEST(BoundedQueue, EnqueueReturnsFalseWhenFull)
{
    MyQueue queue{4};

    EXPECT_TRUE(queue.enqueue(0));
    EXPECT_TRUE(queue.enqueue(1));
    EXPECT_TRUE(queue.enqueue(2));
    EXPECT_TRUE(queue.enqueue(3));
    EXPECT_FALSE(queue.enqueue(4));
}

TEST(BoundedQueue, ReusableAfterDrain)
{
    MyQueue queue{4};

    for (int round = 0; round < 3; ++round)
    {
        EXPECT_TRUE(queue.enqueue(round));
        int value;
        EXPECT_TRUE(queue.dequeue(&value));
    }
    EXPECT_TRUE(queue.empty());
}

TEST(BoundedQueue, ConcurrentEnqueue)
{
    constexpr int N_THREADS = 8;
    constexpr int N_PER_THREAD = 100;
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;

    MyQueue queue{1024};

    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                for (int i = 0; i < N_PER_THREAD; ++i)
                {
                    while (!queue.enqueue(t * N_PER_THREAD + i))
                    {
                        cpuPause();
                    }
                }
            });
    }

    for (auto & thread : threads)
    {
        thread.join();
    }

    int count = 0;
    int discard;
    while (queue.dequeue(&discard))
    {
        ++count;
    }

    EXPECT_EQ(count, N_TOTAL);
}

TEST(BoundedQueue, ConcurrentEnqueueDequeue)
{
    constexpr int N_THREADS = 8;
    constexpr int N_PER_THREAD = 100;
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;

    MyQueue queue{1024};
    std::atomic<int> dequeued{};

    std::vector<std::thread> producers;
    for (int t = 0; t < N_THREADS; ++t)
    {
        producers.emplace_back(
            [&, t]
            {
                for (int i = 0; i < N_PER_THREAD; ++i)
                {
                    while (!queue.enqueue(t * N_PER_THREAD + i))
                    {
                        cpuPause();
                    }
                }
            });
    }

    std::vector<std::thread> consumers;
    for (int t = 0; t < N_THREADS; ++t)
    {
        consumers.emplace_back(
            [&]
            {
                int value;
                while (dequeued.load(std::memory_order_relaxed) < N_TOTAL)
                {
                    if (queue.dequeue(&value))
                    {
                        dequeued.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        cpuPause();
                    }
                }
            });
    }

    for (auto & thread : producers)
    {
        thread.join();
    }
    for (auto & thread : consumers)
    {
        thread.join();
    }

    EXPECT_EQ(dequeued.load(), N_TOTAL);
    EXPECT_TRUE(queue.empty());
}

} // namespace silk
