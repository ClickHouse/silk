#include <silk/util/stack.h>

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

namespace silk
{

struct MyEntry
{
    int value;
    StackEntry stackEntry;

    explicit MyEntry(int value)
        : value(value)
    {
    }
};

using MyStack = LockFreeStack<MyEntry, &MyEntry::stackEntry>;

TEST(LockFreeStack, PopEmptyReturnsNull)
{
    MyStack stack;
    EXPECT_EQ(stack.pop(), nullptr);
}

TEST(LockFreeStack, PushPop)
{
    MyEntry in{42};

    MyStack stack;
    stack.push(&in);

    MyEntry * out = stack.pop();
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->value, 42);
    EXPECT_EQ(stack.pop(), nullptr);
}

TEST(LockFreeStack, LIFOOrder)
{
    MyEntry a{1}, b{2}, c{3};

    MyStack stack;
    stack.push(&a);
    stack.push(&b);
    stack.push(&c);

    EXPECT_EQ(stack.pop()->value, 3);
    EXPECT_EQ(stack.pop()->value, 2);
    EXPECT_EQ(stack.pop()->value, 1);
    EXPECT_EQ(stack.pop(), nullptr);
}

TEST(LockFreeStack, ConcurrentPushPop)
{
    constexpr int N_THREADS = 8;
    constexpr int N_PER_THREAD = 1000;
    constexpr int N_TOTAL = N_THREADS * N_PER_THREAD;

    std::vector<std::unique_ptr<MyEntry>> entries;
    entries.reserve(N_TOTAL);
    for (int i = 0; i < N_TOTAL; ++i)
    {
        entries.push_back(std::make_unique<MyEntry>(0));
    }
    MyStack stack;

    // Push from multiple threads concurrently
    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                for (int i = 0; i < N_PER_THREAD; ++i)
                {
                    stack.push(entries[t * N_PER_THREAD + i].get());
                }
            });
    }

    for (auto & thread : threads)
    {
        thread.join();
    }

    // Pop everything and count
    int count = 0;
    while (stack.pop())
    {
        ++count;
    }

    EXPECT_EQ(count, N_TOTAL);
}

} // namespace silk
