#include <silk/util/bounded-queue.h>

#include <silk/util/assert.h>

#include <benchmark/benchmark.h>

#include <optional>

namespace silk
{

class BoundedQueueBench : public benchmark::Fixture
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
    struct Shared
    {
        static constexpr uint64_t CAPACITY = 1024;
        BoundedQueue<int> queue{CAPACITY};

        Shared()
        {
            for (uint64_t i = 0; i < CAPACITY; ++i)
            {
                bool b = queue.enqueue(i);
                SILK_ASSERT(b);
            }
        }
    };

    std::optional<Shared> shared;
};

BENCHMARK_DEFINE_F(BoundedQueueBench, EnqueueDequeue)(benchmark::State & state)
{
    for (auto _ : state)
    {
        // Enqueue/dequeue can return false spuriously when a concurrent dequeue has claimed
        // a slot but not yet written its sequence number back. Spin until the slot is released.

        int value;
        while (!shared->queue.dequeue(&value))
        {
            cpuPause();
        }

        while (!shared->queue.enqueue(value))
        {
            cpuPause();
        }
    }
}
BENCHMARK_REGISTER_F(BoundedQueueBench, EnqueueDequeue)->ThreadRange(1, getAvailableProcessorCount());

} // namespace silk
