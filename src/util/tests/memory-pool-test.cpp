#include <silk/util/memory-pool.h>

#include <gtest/gtest.h>

#include <thread>
#include <unordered_set>
#include <vector>

namespace silk
{

struct MyObject
{
    StackEntry stackEntry;
    int value = 0;
};

using MyPool = MemoryPool<MyObject, &MyObject::stackEntry>;

TEST(MemoryPool, AllocateReturnsNonNull)
{
    MyPool pool;
    MyObject * object = pool.allocate();

    ASSERT_NE(object, nullptr);

    pool.deallocate(object);
}

TEST(MemoryPool, ObjectIsInitialized)
{
    MyPool pool;
    MyObject * object = pool.allocate();

    ASSERT_NE(object, nullptr);
    EXPECT_EQ(object->value, 0);

    pool.deallocate(object);
}

TEST(MemoryPool, AllocateDeallocateReusesSlot)
{
    MyPool pool;
    constexpr int N = 100;

    std::unordered_set<MyObject *> first;
    for (int i = 0; i < N; ++i)
    {
        first.insert(pool.allocate());
    }
    for (MyObject * object : first)
    {
        pool.deallocate(object);
    }

    // All re-allocations must come from the already-freed slots -- no new chunks.
    std::unordered_set<MyObject *> second;
    for (int i = 0; i < N; ++i)
    {
        second.insert(pool.allocate());
    }
    for (MyObject * object : second)
    {
        EXPECT_TRUE(first.count(object)) << "object was not reused from the free list";
        pool.deallocate(object);
    }
}

TEST(MemoryPool, AllocateMultiple)
{
    MyPool pool;
    constexpr int N = 100;
    std::vector<MyObject *> objects;

    for (int i = 0; i < N; ++i)
    {
        MyObject * object = pool.allocate();
        ASSERT_NE(object, nullptr);
        object->value = i;
        objects.push_back(object);
    }

    for (int i = 0; i < N; ++i)
    {
        EXPECT_EQ(objects[i]->value, i);
    }

    for (MyObject * object : objects)
    {
        pool.deallocate(object);
    }
}

TEST(MemoryPool, AllocationsAreUnique)
{
    MyPool pool;
    constexpr int N = 100;
    std::unordered_set<MyObject *> ptrs;

    for (int i = 0; i < N; ++i)
    {
        ptrs.insert(pool.allocate());
    }

    EXPECT_EQ(static_cast<int>(ptrs.size()), N);

    for (MyObject * object : ptrs)
    {
        pool.deallocate(object);
    }
}

TEST(MemoryPool, AllocationsAreAligned)
{
    MyPool pool;
    constexpr int N = 100;
    std::vector<MyObject *> objects;

    for (int i = 0; i < N; ++i)
    {
        objects.push_back(pool.allocate());
    }

    for (MyObject * object : objects)
    {
        EXPECT_EQ(reinterpret_cast<uintptr_t>(object) % alignof(MyObject), 0u);
        pool.deallocate(object);
    }
}

TEST(MemoryPool, ConcurrentAllocateDeallocate)
{
    constexpr int N_THREADS = 8;
    constexpr int N_PER_THREAD = 1000;

    MyPool pool;

    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t)
    {
        threads.emplace_back(
            [&]
            {
                for (int i = 0; i < N_PER_THREAD; ++i)
                {
                    MyObject * object = pool.allocate();
                    ASSERT_NE(object, nullptr);
                    pool.deallocate(object);
                }
            });
    }

    for (auto & thread : threads)
    {
        thread.join();
    }
}

} // namespace silk
