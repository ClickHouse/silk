#pragma once

#include <silk/util/assert.h>
#include <silk/util/platform.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace silk
{

/**
 * Bounded lock-free MPMC FIFO queue using Dmitry Vyukov's sequence-number algorithm.
 * (https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue)
 *
 * Each slot occupies exactly one cacheline. Capacity must be a power of two and at least 2.
 *
 * Caveat: enqueue and dequeue can return false spuriously even when the queue is not
 * genuinely full or empty. This happens when a concurrent operation has claimed a slot
 * via CAS but has not yet written the updated sequence number back. Callers that cannot
 * tolerate a spurious false must retry in a loop.
 */
template <typename T>
class BoundedQueue
{
public:
    explicit BoundedQueue(uint64_t capacity) noexcept
    {
        SILK_ASSERT(capacity >= 2 && (capacity & (capacity - 1)) == 0);
        mask = capacity - 1;
        slots = std::make_unique<Slot[]>(capacity);
        for (uint64_t i = 0; i < capacity; ++i)
        {
            slots[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    /** Append a value to the tail of the queue. Returns false if the queue is full or
     *  a concurrent dequeue is in progress on the target slot (see class caveat). */
    [[nodiscard]] bool enqueue(T value) noexcept
    {
        uint64_t pos = enqueuePos.load(std::memory_order_relaxed);
        for (;;)
        {
            Slot & slot = slots[pos & mask];
            uint64_t seq = slot.sequence.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
            if (diff == 0)
            {
                if (enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    slot.value = value;
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
            {
                return false;
            }
            else
            {
                pos = enqueuePos.load(std::memory_order_relaxed);
            }
        }
    }

    /** Write the head value into @p value. Returns false if the queue is empty or
     *  a concurrent enqueue is in progress on the target slot (see class caveat). */
    [[nodiscard]] bool dequeue(T * value) noexcept
    {
        uint64_t pos = dequeuePos.load(std::memory_order_relaxed);
        for (;;)
        {
            Slot & slot = slots[pos & mask];
            uint64_t seq = slot.sequence.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1);
            if (diff == 0)
            {
                if (dequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    *value = slot.value;
                    slot.sequence.store(pos + mask + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
            {
                return false;
            }
            else
            {
                pos = dequeuePos.load(std::memory_order_relaxed);
            }
        }
    }

    /**
     * Returns true if the queue appears empty. This is a relaxed check; it may transiently
     * return true while a concurrent enqueue is in progress.
     */
    bool empty() const noexcept
    {
        uint64_t pos = dequeuePos.load(std::memory_order_relaxed);
        uint64_t seq = slots[pos & mask].sequence.load(std::memory_order_acquire);
        return static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1) < 0;
    }

private:
    struct alignas(CACHELINE_SIZE) Slot
    {
        std::atomic<uint64_t> sequence;
        T value;
    };

    static_assert(sizeof(Slot) == CACHELINE_SIZE);

    // Match offsets and stride used by src/gdb/fiber.py::_walk_bounded_queue
    static_assert(offsetof(Slot, sequence) == 0);
    static_assert(offsetof(Slot, value) == 8);

    //
    // State.
    //

    uint64_t mask;
    std::unique_ptr<Slot[]> slots;

    // src/gdb/fiber.py::_walk_bounded_queue reads enqueuePos at offset 64 and
    // dequeuePos at offset 128 (mask=8 bytes, slots=8 bytes, then 2 x cacheline).
    // Reordering or inserting fields here requires updating that script.
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> enqueuePos{};
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> dequeuePos{};
};

} // namespace silk
