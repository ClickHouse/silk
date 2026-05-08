#include <silk/util/sharded-stack.h>

#include <silk/util/assert.h>
#include <silk/util/platform.h>

#include <benchmark/benchmark.h>

#include <optional>
#include <vector>

namespace silk
{

class ShardedStackBench : public benchmark::Fixture
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
    static constexpr uint32_t BATCH_SIZE = 16;

    struct Entry
    {
        int value = 0;
        StackEntry stackEntry;
    };

    using Stack = ShardedStack<Entry, &Entry::stackEntry>;

    struct Shared
    {
        std::vector<Entry> items{getAvailableProcessorCount() * BATCH_SIZE};
        Stack stack{BATCH_SIZE};

        Shared()
        {
            uint64_t capacity = items.size();
            for (uint64_t i = 0; i < capacity;)
            {
                uint64_t batchEnd = std::min(i + BATCH_SIZE, capacity);

                Entry * tail = &items[i];
                Entry * head = tail;

                for (uint64_t j = i + 1; j < batchEnd; ++j)
                {
                    items[j].stackEntry.next.store(&items[j - 1].stackEntry, std::memory_order_relaxed);
                    head = &items[j];
                }

                stack.pushBatch(head, tail);
                i = batchEnd;
            }
        }
    };

    std::optional<Shared> shared;
};

BENCHMARK_DEFINE_F(ShardedStackBench, PushPop)(benchmark::State & state)
{
    for (auto _ : state)
    {
        Entry * entry = shared->stack.pop();
        SILK_ASSERT(entry);

        shared->stack.push(entry);
    }
}
BENCHMARK_REGISTER_F(ShardedStackBench, PushPop)->ThreadRange(1, getAvailableProcessorCount());

} // namespace silk
