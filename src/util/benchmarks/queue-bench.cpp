#include <silk/util/queue.h>

#include <silk/util/assert.h>
#include <silk/util/logger.h>

#include <benchmark/benchmark.h>

#include <optional>
#include <vector>

namespace silk
{

class QueueBench : public benchmark::Fixture
{
public:
    void SetUp(const benchmark::State & state) override
    {
        if (state.thread_index() == 0)
        {
            QueueBase::initialize();
            shared.emplace();
        }
    }

    void TearDown(const benchmark::State & state) override
    {
        if (state.thread_index() == 0)
        {
            shared.reset();
            QueueBase::destroy();
        }
    }

protected:
    struct Item
    {
        int value;
    };

    struct Shared
    {
        static constexpr int CAPACITY = 1024;
        std::vector<Item> items{CAPACITY};
        Queue<Item> queue;

        Shared()
        {
            for (Item & item : items)
            {
                bool b = queue.enqueue(&item);
                SILK_ASSERT(b);
            }
        }
    };

    std::optional<Shared> shared;
};

BENCHMARK_DEFINE_F(QueueBench, EnqueueDequeue)(benchmark::State & state)
{
    for (auto _ : state)
    {
        Item * item = shared->queue.dequeue();
        SILK_ASSERT(item);

        bool b = shared->queue.enqueue(item);
        SILK_ASSERT(b);
    }
}
BENCHMARK_REGISTER_F(QueueBench, EnqueueDequeue)->ThreadRange(1, getAvailableProcessorCount());

class IntrusiveQueueBench : public benchmark::Fixture
{
public:
    void SetUp(const benchmark::State & state) override
    {
        if (state.thread_index() == 0)
        {
            shared.emplace();
        }
    }

    void TearDown(const benchmark::State & state) override
    {
        if (state.thread_index() == 0)
        {
            shared.reset();
        }
    }

protected:
    struct Item
    {
        int value;
        QueueBase::QueueNode queueNode;
        QueueBase::QueueNode * reservedNode = &queueNode;
    };

    struct Shared
    {
        static constexpr uint64_t CAPACITY = 1024;
        std::vector<Item> items{CAPACITY};
        IntrusiveQueue<Item, &Item::reservedNode> queue;

        Shared()
        {
            for (Item & item : items)
            {
                queue.enqueue(&item);
            }
        }
    };

    std::optional<Shared> shared;
};

BENCHMARK_DEFINE_F(IntrusiveQueueBench, EnqueueDequeue)(benchmark::State & state)
{
    for (auto _ : state)
    {
        Item * item = shared->queue.dequeue();
        SILK_ASSERT(item);

        shared->queue.enqueue(item);
    }
}
BENCHMARK_REGISTER_F(IntrusiveQueueBench, EnqueueDequeue)->ThreadRange(1, getAvailableProcessorCount());

} // namespace silk
