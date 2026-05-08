#include <silk/util/spinlock.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace silk
{

TEST(SpinLock, tryLockSucceedsWhenFree)
{
    SpinLock lock;
    EXPECT_TRUE(lock.try_lock());
    lock.unlock();
}

TEST(SpinLock, tryLockFailsWhenHeld)
{
    SpinLock lock;
    EXPECT_TRUE(lock.try_lock());
    EXPECT_FALSE(lock.try_lock());
    lock.unlock();
}

TEST(SpinLock, tryLockSucceedsAfterUnlock)
{
    SpinLock lock;
    lock.lock();
    lock.unlock();
    EXPECT_TRUE(lock.try_lock());
    lock.unlock();
}

TEST(SpinLock, lockUnlock)
{
    SpinLock lock;
    lock.lock();
    lock.unlock();
}

TEST(SpinLock, lockGuardCompatible)
{
    SpinLock lock;
    {
        std::lock_guard guard(lock);
        EXPECT_FALSE(lock.try_lock()); // held by guard
    }
    EXPECT_TRUE(lock.try_lock()); // released by guard
    lock.unlock();
}

TEST(SpinLock, mutualExclusion)
{
    static constexpr int N_THREADS = 8;
    static constexpr int N_ITER = 10000;

    SpinLock lock;
    int counter = 0;

    auto worker = [&]
    {
        for (int i = 0; i < N_ITER; ++i)
        {
            std::lock_guard guard(lock);
            ++counter;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (int i = 0; i < N_THREADS; ++i)
    {
        threads.emplace_back(worker);
    }
    for (auto & thread : threads)
    {
        thread.join();
    }

    ASSERT_EQ(counter, N_THREADS * N_ITER);
}

TEST(SpinWait, returnsTrueWhenPredicateSatisfied)
{
    std::atomic<bool> flag{false};
    std::thread setter([&] { flag.store(true, std::memory_order_release); });

    bool result = spinWait([&] { return flag.load(std::memory_order_acquire); }, SPIN_COUNT);
    setter.join();

    // Either the spin caught it or the predicate was satisfied — the flag is true.
    EXPECT_TRUE(flag.load());
    SILK_UNUSED(result);
}

TEST(SpinWait, returnsFalseWhenExhausted)
{
    // Predicate never becomes true.
    bool result = spinWait([] { return false; }, 8);
    EXPECT_FALSE(result);
}

TEST(SpinWait, returnsTrueImmediately)
{
    bool result = spinWait([] { return true; }, SPIN_COUNT);
    EXPECT_TRUE(result);
}

} // namespace silk
