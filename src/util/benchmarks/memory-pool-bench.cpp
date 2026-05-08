#include <silk/util/memory-pool.h>

#include <silk/util/assert.h>

#include <benchmark/benchmark.h>

#include <optional>
#include <vector>

namespace silk
{

class MemoryPoolBench : public benchmark::Fixture
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
    struct PoolObject
    {
        StackEntry stackEntry;
        uint64_t value = 0;
    };

    struct Shared
    {
        MemoryPool<PoolObject, &PoolObject::stackEntry> pool;
    };

    std::optional<Shared> shared;
};

BENCHMARK_DEFINE_F(MemoryPoolBench, AllocFreeBatch)(benchmark::State & state)
{
    uint64_t batchSize = state.range(0);
    std::vector<PoolObject *> objects(batchSize);

    for (auto _ : state)
    {
        for (uint64_t i = 0; i < batchSize; ++i)
        {
            objects[i] = shared->pool.allocate();
        }
        for (uint64_t i = 0; i < batchSize; ++i)
        {
            shared->pool.deallocate(objects[i]);
        }
    }

    state.SetItemsProcessed(state.iterations() * batchSize * 2);
}
BENCHMARK_REGISTER_F(MemoryPoolBench, AllocFreeBatch)->Arg(16)->Arg(64)->Arg(256);

BENCHMARK_DEFINE_F(MemoryPoolBench, AllocFree)(benchmark::State & state)
{
    for (auto _ : state)
    {
        PoolObject * obj = shared->pool.allocate();
        SILK_ASSERT(obj);

        shared->pool.deallocate(obj);
    }
}
BENCHMARK_REGISTER_F(MemoryPoolBench, AllocFree)->ThreadRange(1, getAvailableProcessorCount());

} // namespace silk
