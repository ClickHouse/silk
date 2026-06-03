#pragma once

#include <silk/util/assert.h>
#include <silk/util/platform.h>

#include <atomic>
#include <cstdint>
#include <memory>

#define DECLARE_COUNTER_ID(id, name) id,
#define DECLARE_COUNTER_NAME(id, name) name,

/**
 * Declare a local enum mapping counter names to per-group indices.
 *
 * Usage:
 *   #define MY_COUNTERS(x) \
 *       x(REQUESTS, "Requests") \
 *       x(ERRORS,   "Errors")
 *   DECLARE_SIMPLE_COUNTERS(MY_COUNTERS);
 *
 * Produces: enum SimpleCounters { REQUESTS, ERRORS };
 */
#define DECLARE_SIMPLE_COUNTERS(counters) \
    enum SimpleCounters : uint32_t \
    { \
        counters(DECLARE_COUNTER_ID) \
    }

/**
 * Register all counters declared by a DECLARE_SIMPLE_COUNTERS macro and
 * bind them to a CounterGroup. Asserts on allocation failure (out of slots).
 * Must be called after Perf::initialize and before any increment.
 */
#define REGISTER_SIMPLE_COUNTERS(ptr, counters) \
    do \
    { \
        static const char * names[] = {counters(DECLARE_COUNTER_NAME)}; \
        int r = silk::Perf::registerSimpleCounters(ptr, names, std::size(names)); \
        SILK_ASSERT(!r); \
    } while (0)

/**
 * Declare a local enum mapping memory counter names to per-group indices.
 * Usage mirrors DECLARE_SIMPLE_COUNTERS.
 */
#define DECLARE_MEM_COUNTERS(counters) \
    enum MemCounters : uint32_t \
    { \
        counters(DECLARE_COUNTER_ID) \
    }

/**
 * Register all counters declared by a DECLARE_MEM_COUNTERS macro and
 * bind them to a CounterGroup. Asserts on allocation failure (out of slots).
 * Must be called after Perf::initialize and before any increment.
 */
#define REGISTER_MEM_COUNTERS(ptr, counters) \
    do \
    { \
        static const char * names[] = {counters(DECLARE_COUNTER_NAME)}; \
        int r = silk::Perf::registerMemCounters(ptr, names, std::size(names)); \
        SILK_ASSERT(!r); \
    } while (0)

namespace silk
{

/**
 * Per-CPU performance counter registry.
 *
 * Each counter has one slot per CPU so increments are contention-free
 * (relaxed fetch_add with no cross-CPU traffic). Reading is done by
 * accumulating all per-CPU slots into a flat output array, which is
 * intentionally expensive and expected to happen rarely (e.g. at
 * shutdown or on a stats endpoint).
 *
 * Lifecycle: call initialize once before use, destroy once after.
 * Components register their counters via REGISTER_SIMPLE_COUNTERS at
 * startup; counter indices are stable for the lifetime of the process.
 */
class Perf
{
public:
    static constexpr uint32_t NUM_SIMPLE_COUNTERS = 1000;
    static constexpr uint32_t NUM_MEM_COUNTERS = 1000;

    /**
     * Allocate per-CPU counter arrays. Must be called before any other method.
     */
    static void initialize() noexcept;

    /**
     * Release all resources. After this call initialize may be called again.
     */
    static void destroy() noexcept;

    /**
     * Metadata stored alongside each registered counter slot.
     */
    struct CounterInfo
    {
        const char * name = nullptr;
    };

    /**
     * A single per-CPU counter. All operations are relaxed atomics;
     * there is no happens-before guarantee between CPUs.
     */
    struct SimpleCounter
    {
        void reset() noexcept { value.store(0, std::memory_order_relaxed); }

        void increment(uint64_t v = 1) noexcept { value.fetch_add(v, std::memory_order_relaxed); }

        void accumulate(const SimpleCounter & c) noexcept
        {
            value.store(value.load(std::memory_order_relaxed) + c.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }

        void flush(SimpleCounter & c) noexcept
        {
            c.value.fetch_add(value.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
        }

        std::atomic<uint64_t> value;
    };

    /**
     * Per-CPU counter tracking allocation and deallocation activity.
     * All operations are relaxed atomics; there is no happens-before
     * guarantee between CPUs.
     */
    struct MemCounter
    {
        void reset() noexcept
        {
            allocCount.store(0, std::memory_order_relaxed);
            allocSize.store(0, std::memory_order_relaxed);
            deallocCount.store(0, std::memory_order_relaxed);
            deallocSize.store(0, std::memory_order_relaxed);
        }

        void alloc(uint64_t byteSize) noexcept
        {
            allocCount.fetch_add(1, std::memory_order_relaxed);
            allocSize.fetch_add(byteSize, std::memory_order_relaxed);
        }

        void dealloc(uint64_t byteSize) noexcept
        {
            deallocCount.fetch_add(1, std::memory_order_relaxed);
            deallocSize.fetch_add(byteSize, std::memory_order_relaxed);
        }

        void accumulate(const MemCounter & c) noexcept
        {
            allocCount.store(
                allocCount.load(std::memory_order_relaxed) + c.allocCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
            allocSize.store(
                allocSize.load(std::memory_order_relaxed) + c.allocSize.load(std::memory_order_relaxed), std::memory_order_relaxed);
            deallocCount.store(
                deallocCount.load(std::memory_order_relaxed) + c.deallocCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
            deallocSize.store(
                deallocSize.load(std::memory_order_relaxed) + c.deallocSize.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }

        void flush(MemCounter & c) noexcept
        {
            c.allocCount.fetch_add(allocCount.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
            c.allocSize.fetch_add(allocSize.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
            c.deallocCount.fetch_add(deallocCount.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
            c.deallocSize.fetch_add(deallocSize.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
        }

        std::atomic<uint64_t> allocCount;
        std::atomic<uint64_t> allocSize;
        std::atomic<uint64_t> deallocCount;
        std::atomic<uint64_t> deallocSize;
    };

    /**
     * Handle returned by registerSimpleCounters. Translates a local
     * per-group index (from the DECLARE_SIMPLE_COUNTERS enum) to the
     * global slot index used by getSimpleCounter.
     */
    struct CounterGroup
    {
        uint32_t operator[](uint32_t index) const noexcept { return startIndex + index; }

        uint32_t startIndex = 0;
        uint32_t count = 0;
    };

    /**
     * Atomically allocate @p count consecutive global slots and bind them
     * to @p group. Records @p names for introspection. Returns 0 on success,
     * ENOMEM if the global slot pool is exhausted.
     */
    static int registerSimpleCounters(CounterGroup * group, const char ** names, uint32_t count) noexcept;

    /**
     * Convenience overload: allocate a single slot and bind it to @p group.
     * Returns 0 on success, ENOMEM if the global slot pool is exhausted.
     */
    static int registerSimpleCounter(CounterGroup * group, const char * name) noexcept { return registerSimpleCounters(group, &name, 1); }

    /**
     * Total number of registered counter slots across all groups.
     */
    static uint32_t getSimpleCounterCount() noexcept { return simpleCounterCount.load(std::memory_order_relaxed); }

    /**
     * Metadata (name) for the counter at global slot @p index.
     */
    static const CounterInfo & getSimpleCounterInfo(uint32_t index) noexcept { return simpleCounters[index]; }

    /**
     * Per-CPU counter at global slot @p index for CPU @p cpu.
     * Defaults to the calling thread's current CPU.
     */
    [[nodiscard]] static SimpleCounter & getSimpleCounter(uint32_t index, uint32_t cpu = getCurrentProcessor()) noexcept
    {
        return processorState[cpu].simpleCounters[index];
    }

    /**
     * Accumulate simple counter slots [@p index, @p index + @p counterArraySize)
     * across every CPU into @p counterArray. Returns the number of slots written.
     * Each output entry is zeroed before accumulation.
     */
    [[nodiscard]] static uint32_t getSimpleCounters(uint32_t index, SimpleCounter * counterArray, uint32_t counterArraySize) noexcept;

    /**
     * Atomically allocate @p count consecutive global slots and bind them
     * to @p group. Records @p names for introspection. Returns 0 on success,
     * ENOMEM if the global slot pool is exhausted.
     */
    static int registerMemCounters(CounterGroup * group, const char ** names, uint32_t count) noexcept;

    /**
     * Convenience overload: allocate a single slot and bind it to @p group.
     * Returns 0 on success, ENOMEM if the global slot pool is exhausted.
     */
    static int registerMemCounter(CounterGroup * group, const char * name) noexcept { return registerMemCounters(group, &name, 1); }

    /**
     * Total number of registered memory counter slots across all groups.
     */
    static uint32_t getMemCounterCount() noexcept { return memCounterCount.load(std::memory_order_relaxed); }

    /**
     * Metadata (name) for the memory counter at global slot @p index.
     */
    static const CounterInfo & getMemCounterInfo(uint32_t index) noexcept { return memCounters[index]; }

    /**
     * Per-CPU memory counter at global slot @p index for CPU @p cpu.
     * Defaults to the calling thread's current CPU.
     */
    [[nodiscard]] static MemCounter & getMemCounter(uint32_t index, uint32_t cpu = getCurrentProcessor()) noexcept
    {
        return processorState[cpu].memCounters[index];
    }

    /**
     * Accumulate memory counter slots [@p index, @p index + @p counterArraySize)
     * across every CPU into @p counterArray. Returns the number of slots written.
     * Each output entry is zeroed before accumulation.
     */
    [[nodiscard]] static uint32_t getMemCounters(uint32_t index, MemCounter * counterArray, uint32_t counterArraySize) noexcept;

private:
    struct ProcessorState
    {
        std::unique_ptr<SimpleCounter[]> simpleCounters;
        std::unique_ptr<MemCounter[]> memCounters;
    };

    //
    // State.
    //

    static inline std::unique_ptr<ProcessorState[]> processorState;

    static inline std::atomic<uint32_t> simpleCounterCount;
    static inline std::unique_ptr<CounterInfo[]> simpleCounters;

    static inline std::atomic<uint32_t> memCounterCount;
    static inline std::unique_ptr<CounterInfo[]> memCounters;
};

} // namespace silk
