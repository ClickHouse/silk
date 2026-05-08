#include <silk/util/stack.h>

#include <silk/util/assert.h>
#include <silk/util/platform.h>
#include <silk/util/spinlock.h>

#include <benchmark/benchmark.h>

#include <mutex>
#include <optional>
#include <vector>

namespace silk
{

class LockFreeStackBench : public benchmark::Fixture
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
    struct Entry
    {
        StackEntry stackEntry;
        int value = 0;
    };

    using MyStack = LockFreeStack<Entry, &Entry::stackEntry>;

    struct Shared
    {
        static constexpr uint64_t CAPACITY = 1024;
        std::vector<Entry> items{CAPACITY};
        MyStack stack;

        Shared()
        {
            for (Entry & entry : items)
            {
                stack.push(&entry);
            }
        }
    };

    std::optional<Shared> shared;
};

BENCHMARK_DEFINE_F(LockFreeStackBench, PushPop)(benchmark::State & state)
{
    for (auto _ : state)
    {
        Entry * entry = shared->stack.pop();
        SILK_ASSERT(entry);

        shared->stack.push(entry);
    }
}
BENCHMARK_REGISTER_F(LockFreeStackBench, PushPop)->ThreadRange(1, getAvailableProcessorCount());

class StackBench : public benchmark::Fixture
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
    struct Entry
    {
        StackEntry stackEntry;
        int value = 0;
    };

    using MyStack = Stack<Entry, &Entry::stackEntry>;

    struct Shared
    {
        static constexpr uint64_t CAPACITY = 1024;
        std::vector<Entry> items{CAPACITY};
        SpinLock lock;
        MyStack stack;

        Shared()
        {
            for (Entry & entry : items)
            {
                stack.push(&entry);
            }
        }
    };

    std::optional<Shared> shared;
};

BENCHMARK_DEFINE_F(StackBench, PushPop)(benchmark::State & state)
{
    for (auto _ : state)
    {
        std::lock_guard lock(shared->lock);

        Entry * entry = shared->stack.pop();
        SILK_ASSERT(entry);

        shared->stack.push(entry);
    }
}
BENCHMARK_REGISTER_F(StackBench, PushPop)->ThreadRange(1, getAvailableProcessorCount());

BENCHMARK_F(StackBench, LocalPushPop)(benchmark::State & state)
{
    MyStack stack;

    Entry entry{};
    stack.push(&entry);

    for (auto _ : state)
    {
        stack.push(stack.pop());
    }

    benchmark::DoNotOptimize(entry);
}

} // namespace silk
