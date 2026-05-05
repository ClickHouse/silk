#include <silk/util/queue.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace silk
{

class QueueTest : public ::testing::Test
{
public:
    void SetUp() override { QueueBase::initialize(); }
    void TearDown() override { QueueBase::destroy(); }

protected:
    using MyQueue = Queue<int>;
};

TEST_F(QueueTest, DequeueEmptyReturnsNull)
{
    MyQueue queue;
    EXPECT_EQ(queue.dequeue(), nullptr);
}

TEST_F(QueueTest, EnqueueDequeue)
{
    MyQueue queue;
    int value = 42;

    ASSERT_TRUE(queue.enqueue(&value));

    int * result = queue.dequeue();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result, &value);
    EXPECT_EQ(queue.dequeue(), nullptr);
}

TEST_F(QueueTest, FIFOOrder)
{
    MyQueue queue;
    int a = 1, b = 2, c = 3;

    ASSERT_TRUE(queue.enqueue(&a));
    ASSERT_TRUE(queue.enqueue(&b));
    ASSERT_TRUE(queue.enqueue(&c));

    EXPECT_EQ(queue.dequeue(), &a);
    EXPECT_EQ(queue.dequeue(), &b);
    EXPECT_EQ(queue.dequeue(), &c);
    EXPECT_EQ(queue.dequeue(), nullptr);
}

TEST_F(QueueTest, ConcurrentEnqueue)
{
    constexpr int N_THREADS = 8;
    constexpr int N_PER_THREAD = 1000;
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;

    std::vector<int> values(N_TOTAL);
    MyQueue queue;

    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                for (int i = 0; i < N_PER_THREAD; ++i)
                {
                    EXPECT_TRUE(queue.enqueue(&values[t * N_PER_THREAD + i]));
                }
            });
    }

    for (auto & thread : threads)
    {
        thread.join();
    }

    int count = 0;
    while (queue.dequeue())
    {
        ++count;
    }

    EXPECT_EQ(count, N_TOTAL);
}

TEST_F(QueueTest, ConcurrentEnqueueDequeue)
{
    constexpr int N_THREADS = 8;
    constexpr int N_PER_THREAD = 1000;
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;

    std::vector<int> values(N_TOTAL);
    MyQueue queue;
    std::atomic<int> dequeued{};

    std::vector<std::thread> producers;
    for (int t = 0; t < N_THREADS; ++t)
    {
        producers.emplace_back(
            [&, t]
            {
                for (int i = 0; i < N_PER_THREAD; ++i)
                {
                    EXPECT_TRUE(queue.enqueue(&values[t * N_PER_THREAD + i]));
                }
            });
    }

    std::vector<std::thread> consumers;
    for (int t = 0; t < N_THREADS; ++t)
    {
        consumers.emplace_back(
            [&]
            {
                while (dequeued.load(std::memory_order_relaxed) < N_TOTAL)
                {
                    if (queue.dequeue())
                    {
                        dequeued.fetch_add(1, std::memory_order_relaxed);
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
    EXPECT_EQ(queue.dequeue(), nullptr);
}

class IntrusiveQueueTest : public ::testing::Test
{
protected:
    struct Item
    {
        int value = 0;
        QueueBase::QueueNode queueNode;
        QueueBase::QueueNode * reservedNode = &queueNode;
    };

    using MyQueue = IntrusiveQueue<Item, &Item::reservedNode>;
};

TEST_F(IntrusiveQueueTest, DequeueEmptyReturnsNull)
{
    MyQueue queue;
    EXPECT_EQ(queue.dequeue(), nullptr);
}

TEST_F(IntrusiveQueueTest, EnqueueDequeue)
{
    MyQueue queue;
    Item item;

    queue.enqueue(&item);

    Item * result = queue.dequeue();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result, &item);
    EXPECT_EQ(queue.dequeue(), nullptr);
}

TEST_F(IntrusiveQueueTest, FIFOOrder)
{
    MyQueue queue;
    Item a, b, c;

    queue.enqueue(&a);
    queue.enqueue(&b);
    queue.enqueue(&c);

    EXPECT_EQ(queue.dequeue(), &a);
    EXPECT_EQ(queue.dequeue(), &b);
    EXPECT_EQ(queue.dequeue(), &c);
    EXPECT_EQ(queue.dequeue(), nullptr);
}

// After dequeue, reservedNode is updated to the recycled sentinel so the item
// can be re-enqueued immediately without any external bookkeeping.
TEST_F(IntrusiveQueueTest, ReenqueueAfterDequeue)
{
    MyQueue queue;
    Item item;

    for (int i = 0; i < 8; ++i)
    {
        queue.enqueue(&item);
        Item * result = queue.dequeue();
        ASSERT_EQ(result, &item);
        EXPECT_EQ(queue.dequeue(), nullptr);
    }
}

// Mix two items: alternate enqueue/dequeue to exercise the recycled-sentinel
// handoff between items (reservedNode cycles between each item's queueNode
// and the queue's initial dummy).
TEST_F(IntrusiveQueueTest, ReenqueueTwoItems)
{
    MyQueue queue;
    Item a, b;

    for (int i = 0; i < 8; ++i)
    {
        queue.enqueue(&a);
        queue.enqueue(&b);
        EXPECT_EQ(queue.dequeue(), &a);
        EXPECT_EQ(queue.dequeue(), &b);
        EXPECT_EQ(queue.dequeue(), nullptr);
    }
}

TEST_F(IntrusiveQueueTest, ConcurrentEnqueue)
{
    constexpr int N_THREADS = 8;
    constexpr int N_PER_THREAD = 1000;
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;

    std::vector<Item> items(N_TOTAL);
    MyQueue queue;

    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                for (int i = 0; i < N_PER_THREAD; ++i)
                {
                    queue.enqueue(&items[t * N_PER_THREAD + i]);
                }
            });
    }

    for (auto & thread : threads)
    {
        thread.join();
    }

    int count = 0;
    while (queue.dequeue())
    {
        ++count;
    }

    EXPECT_EQ(count, N_TOTAL);
}

TEST_F(IntrusiveQueueTest, ConcurrentEnqueueDequeue)
{
    constexpr int N_THREADS = 8;
    constexpr int N_PER_THREAD = 1000;
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;

    std::vector<Item> items(N_TOTAL);
    MyQueue queue;
    std::atomic<int> dequeued{};

    std::vector<std::thread> producers;
    for (int t = 0; t < N_THREADS; ++t)
    {
        producers.emplace_back(
            [&, t]
            {
                for (int i = 0; i < N_PER_THREAD; ++i)
                {
                    queue.enqueue(&items[t * N_PER_THREAD + i]);
                }
            });
    }

    std::vector<std::thread> consumers;
    for (int t = 0; t < N_THREADS; ++t)
    {
        consumers.emplace_back(
            [&]
            {
                while (dequeued.load(std::memory_order_relaxed) < N_TOTAL)
                {
                    if (queue.dequeue())
                    {
                        dequeued.fetch_add(1, std::memory_order_relaxed);
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
    EXPECT_EQ(queue.dequeue(), nullptr);
}

} // namespace silk
