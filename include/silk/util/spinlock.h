#pragma once

#include <silk/util/platform.h>

#include <atomic>
#include <cstdint>

namespace silk
{

// Spin for ~2 us (64 x ~35 ns PAUSE on Skylake) before yielding.
// Chosen to cover roughly one OS context-switch latency (~1-10 us).
static constexpr uint32_t SPIN_COUNT = 64;

/**
 * Spin up to @p spinCount times checking @p pred after each cpuPause().
 * Returns true if pred returned true, false if the spin limit was exhausted.
 */
template <typename Predicate>
static inline bool spinWait(Predicate && pred, uint32_t spinCount = SPIN_COUNT) noexcept
{
    for (uint32_t i = 0; i < spinCount; ++i)
    {
        cpuPause();
        if (pred())
        {
            return true;
        }
    }
    return false;
}

/**
 * Spinlock with two-phase backoff: spin with cpuPause() for SPIN_COUNT
 * iterations, then call schedYield() if the lock is still held, and repeat.
 */
class SpinLock
{
public:
    [[nodiscard]] bool try_lock() noexcept
    {
        return !flag.load(std::memory_order_relaxed) && !flag.exchange(true, std::memory_order_acquire);
    }

    void lock() noexcept
    {
        // Fast path: one CAS. Inlines into every caller so the uncontended
        // case is a single atomic op + branch. Contention falls through to
        // lockSlow, which is kept out of the inliner's reach.
        if (!try_lock()) [[unlikely]]
        {
            lockSlow();
        }
    }

    void unlock() noexcept { flag.store(false, std::memory_order_release); }

private:
    __attribute__((noinline)) void lockSlow() noexcept
    {
        while (!try_lock())
        {
            if (!spinWait([this] { return !flag.load(std::memory_order_relaxed); }))
            {
                schedYield();
            }
        }
    }

    std::atomic<bool> flag{false};
};

} // namespace silk
