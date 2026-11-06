#include <silk/util/sharded-stack.h>

#include <silk/util/platform.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <vector>

namespace silk
{

struct TestEntry
{
    int value;
    StackEntry stackEntry;
};

using TestStack = ShardedStack<TestEntry, &TestEntry::stackEntry>;

// -------------------------------------------------------------------------
// Basic
// -------------------------------------------------------------------------

TEST(ShardedStack, PopEmptyReturnsNull)
{
    TestStack stack{4};
    EXPECT_EQ(stack.pop(), nullptr);
}

TEST(ShardedStack, PushPop)
{
    TestStack stack{4};
    TestEntry entry{42, {}};
    stack.push(&entry);

    TestEntry * popped = stack.pop();
    ASSERT_NE(popped, nullptr);
    EXPECT_EQ(popped->value, 42);
    EXPECT_EQ(stack.pop(), nullptr);
}

TEST(ShardedStack, AllEntriesRecovered)
{
    constexpr int N = 100;
    TestStack stack{8};

    std::vector<TestEntry> entries(N);
    for (int i = 0; i < N; ++i)
    {
        entries[i].value = i;
        stack.push(&entries[i]);
    }

    std::unordered_set<int> recovered;
    stack.drain(
        [](TestEntry * entry, void * raw) noexcept
        { EXPECT_TRUE(static_cast<std::unordered_set<int> *>(raw)->insert(entry->value).second) << "duplicate: " << entry->value; },
        &recovered);
    EXPECT_EQ(recovered.size(), N);
}

// -------------------------------------------------------------------------
// Slow path coverage
// -------------------------------------------------------------------------

TEST(ShardedStack, SlowPathEveryOperation)
{
    // batchSize=1 forces a slow-path trip on every push and pop.
    constexpr int N = 64;
    TestStack stack{1};

    std::vector<TestEntry> entries(N);
    for (int i = 0; i < N; ++i)
    {
        entries[i].value = i;
        stack.push(&entries[i]);
    }

    std::unordered_set<int> recovered;
    stack.drain(
        [](TestEntry * entry, void * raw) noexcept { static_cast<std::unordered_set<int> *>(raw)->insert(entry->value); }, &recovered);
    EXPECT_EQ(recovered.size(), N);
}

TEST(ShardedStack, ExhaustAndRefill)
{
    constexpr int N = 64;
    TestStack stack{8};

    std::vector<TestEntry> entries(N);
    for (int i = 0; i < N; ++i)
    {
        entries[i].value = i;
        stack.push(&entries[i]);
    }

    // Drain completely.
    std::vector<TestEntry *> drained;
    stack.drain([](TestEntry * entry, void * raw) noexcept { static_cast<std::vector<TestEntry *> *>(raw)->push_back(entry); }, &drained);
    ASSERT_EQ(drained.size(), N);
    EXPECT_EQ(stack.pop(), nullptr);

    // Push all back and pop again.
    for (TestEntry * e : drained)
    {
        stack.push(e);
    }

    std::unordered_set<int> recovered;
    stack.drain(
        [](TestEntry * entry, void * raw) noexcept { static_cast<std::unordered_set<int> *>(raw)->insert(entry->value); }, &recovered);
    EXPECT_EQ(recovered.size(), N);
}

// -------------------------------------------------------------------------
// Concurrent
// -------------------------------------------------------------------------

TEST(ShardedStack, ConcurrentPush)
{
    constexpr int N_THREADS = 8;
    constexpr int N_PER_THREAD = 500;
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;

    TestStack stack{16};
    std::vector<TestEntry> entries(N_TOTAL);
    for (int i = 0; i < N_TOTAL; ++i)
    {
        entries[i].value = i;
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                for (int i = 0; i < N_PER_THREAD; ++i)
                {
                    stack.push(&entries[t * N_PER_THREAD + i]);
                }
            });
    }
    for (auto & thread : threads)
    {
        thread.join();
    }

    std::unordered_set<int> recovered;
    stack.drain(
        [](TestEntry * entry, void * raw) noexcept
        { EXPECT_TRUE(static_cast<std::unordered_set<int> *>(raw)->insert(entry->value).second) << "duplicate: " << entry->value; },
        &recovered);
    EXPECT_EQ(recovered.size(), N_TOTAL);
}

TEST(ShardedStack, ConcurrentPushPop)
{
    constexpr int N_THREADS = 8;
    constexpr int N_PER_THREAD = 500;
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;

    TestStack stack{16};
    std::vector<TestEntry> entries(N_TOTAL);
    for (int i = 0; i < N_TOTAL; ++i)
    {
        entries[i].value = i;
        stack.push(&entries[i]);
    }

    std::atomic<int> totalPopped{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t)
    {
        threads.emplace_back(
            [&]
            {
                int count = 0;
                while (stack.pop())
                {
                    ++count;
                }
                totalPopped.fetch_add(count, std::memory_order_relaxed);
            });
    }
    for (auto & thread : threads)
    {
        thread.join();
    }

    stack.drain(
        [](TestEntry *, void * raw) noexcept { static_cast<std::atomic<int> *>(raw)->fetch_add(1, std::memory_order_relaxed); },
        &totalPopped);

    EXPECT_EQ(totalPopped.load(), N_TOTAL);
}

TEST(ShardedStack, ConcurrentInterleavedNoLoss)
{
    // Each thread pops an entry and pushes it back. No entries must be lost.
    constexpr int N_THREADS = 4;
    constexpr int N_ROUNDS = 500;
    constexpr int N_ENTRIES = 32;

    TestStack stack{8};
    std::vector<TestEntry> entries(N_ENTRIES);
    for (int i = 0; i < N_ENTRIES; ++i)
    {
        entries[i].value = i;
        stack.push(&entries[i]);
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t)
    {
        threads.emplace_back(
            [&]
            {
                for (int i = 0; i < N_ROUNDS; ++i)
                {
                    TestEntry * entry = stack.pop();
                    if (entry)
                    {
                        stack.push(entry);
                    }
                }
            });
    }
    for (auto & thread : threads)
    {
        thread.join();
    }

    // All entries must still be recoverable after all threads complete.
    std::unordered_set<int> recovered;
    stack.drain(
        [](TestEntry * entry, void * raw) noexcept { static_cast<std::unordered_set<int> *>(raw)->insert(entry->value); }, &recovered);
    EXPECT_EQ(recovered.size(), N_ENTRIES);
}

TEST(ShardedStack, ConcurrentSmallBatch)
{
    // batchSize=1 under concurrency stresses the slow path and FreeLists pool.
    constexpr int N_THREADS = 4;
    constexpr int N_PER_THREAD = 200;
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;

    TestStack stack{1};
    std::vector<TestEntry> entries(N_TOTAL);
    for (int i = 0; i < N_TOTAL; ++i)
    {
        entries[i].value = i;
        stack.push(&entries[i]);
    }

    std::atomic<int> totalPopped{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t)
    {
        threads.emplace_back(
            [&]
            {
                int count = 0;
                while (stack.pop())
                {
                    ++count;
                }
                totalPopped.fetch_add(count, std::memory_order_relaxed);
            });
    }
    for (auto & thread : threads)
    {
        thread.join();
    }

    stack.drain(
        [](TestEntry *, void * raw) noexcept { static_cast<std::atomic<int> *>(raw)->fetch_add(1, std::memory_order_relaxed); },
        &totalPopped);

    EXPECT_EQ(totalPopped.load(), N_TOTAL);
}

TEST(ShardedStack, ConcurrentMigrationNoLoss)
{
    constexpr int N_THREADS = 8;
    constexpr int N_ENTRIES = 256;
    constexpr int N_CPUS = 4;

    cpu_set_t affinitySet;
    int r = sched_getaffinity(0, sizeof(cpu_set_t), &affinitySet);
    ASSERT_FALSE(r);

    int cpus[N_CPUS];
    int cpuCount = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && cpuCount < N_CPUS; ++cpu)
    {
        if (CPU_ISSET(cpu, &affinitySet))
        {
            cpus[cpuCount++] = cpu;
        }
    }

    if (cpuCount < 2)
    {
        GTEST_SKIP() << "the migration needs two CPUs";
    }

    // batchSize=1 sends every operation to a slow path.
    TestStack stack{1};
    std::vector<TestEntry> entries(N_ENTRIES);
    for (int i = 0; i < N_ENTRIES; ++i)
    {
        entries[i].value = i;
        stack.push(&entries[i]);
    }

    std::atomic<bool> stopped{false};
    std::atomic<bool> migratorStopped{false};
    std::atomic<pid_t> threadIds[N_THREADS] = {};
    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                threadIds[t].store(gettid());
                while (!stopped.load(std::memory_order_relaxed))
                {
                    if (TestEntry * entry = stack.pop())
                    {
                        stack.push(entry);
                    }
                }
            });
    }

    // Continuously re-pin threads to random CPUs.
    std::thread migrator(
        [&]
        {
            for (uint64_t step = 0; !migratorStopped.load(std::memory_order_relaxed); ++step)
            {
                cpu_set_t cpuSet;
                CPU_ZERO(&cpuSet);
                CPU_SET(cpus[intHash(step) % cpuCount], &cpuSet);
                if (pid_t threadId = threadIds[step % N_THREADS].load())
                {
                    int r = sched_setaffinity(threadId, sizeof(cpu_set_t), &cpuSet);
                    SILK_ASSERT(!r, "could not re-pin a thread: errno=%d", errno);
                }
            }
        });

    std::this_thread::sleep_for(std::chrono::seconds(2));
    migratorStopped.store(true);
    migrator.join();
    stopped.store(true);
    for (auto & thread : threads)
    {
        thread.join();
    }

    std::unordered_set<int> recovered;
    stack.drain(
        [](TestEntry * entry, void * raw) noexcept
        { EXPECT_TRUE(static_cast<std::unordered_set<int> *>(raw)->insert(entry->value).second) << "duplicate: " << entry->value; },
        &recovered);
    ASSERT_EQ(recovered.size(), N_ENTRIES);
}

} // namespace silk
