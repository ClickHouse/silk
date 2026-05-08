#include <silk/util/perf.h>

#include <silk/util/platform.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <memory>

namespace silk
{

void Perf::initialize() noexcept
{
    if (processorState)
    {
        // Skip the second initialization.
        return;
    }

    uint32_t processorCount = getProcessorCount();

    simpleCounters = std::make_unique<CounterInfo[]>(NUM_SIMPLE_COUNTERS);
    memCounters = std::make_unique<CounterInfo[]>(NUM_MEM_COUNTERS);

    processorState = std::make_unique<ProcessorState[]>(processorCount);
    for (uint32_t cpu = 0; cpu < processorCount; ++cpu)
    {
        processorState[cpu].simpleCounters = std::make_unique<SimpleCounter[]>(NUM_SIMPLE_COUNTERS);
        processorState[cpu].memCounters = std::make_unique<MemCounter[]>(NUM_MEM_COUNTERS);
    }
}

void Perf::destroy() noexcept
{
    simpleCounters.reset();
    memCounters.reset();
    processorState.reset();
    simpleCounterCount.store(0, std::memory_order_relaxed);
    memCounterCount.store(0, std::memory_order_relaxed);
}

int Perf::registerSimpleCounters(CounterGroup * group, const char ** names, uint32_t count) noexcept
{
    SILK_ASSERT(group->count == 0);

    uint32_t base = simpleCounterCount.load(std::memory_order_relaxed);
    for (;;)
    {
        if (base + count > NUM_SIMPLE_COUNTERS)
        {
            return ENOMEM;
        }

        if (simpleCounterCount.compare_exchange_weak(base, base + count, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            break;
        }
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        simpleCounters[base + i].name = names[i];
    }

    group->startIndex = base;
    group->count = count;
    return 0;
}

uint32_t Perf::getSimpleCounters(uint32_t index, SimpleCounter * counterArray, uint32_t counterArraySize) noexcept
{
    uint32_t totalCount = simpleCounterCount.load(std::memory_order_relaxed);
    if (index >= totalCount)
    {
        return 0;
    }

    uint32_t count = std::min(counterArraySize, totalCount - index);
    for (uint32_t i = 0; i < count; ++i)
    {
        counterArray[i].reset();
    }

    uint32_t processorCount = getProcessorCount();
    for (uint32_t cpu = 0; cpu < processorCount; ++cpu)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            counterArray[i].accumulate(processorState[cpu].simpleCounters[index + i]);
        }
    }

    return count;
}

int Perf::registerMemCounters(CounterGroup * group, const char ** names, uint32_t count) noexcept
{
    SILK_ASSERT(group->count == 0);

    uint32_t base = memCounterCount.load(std::memory_order_relaxed);
    for (;;)
    {
        if (base + count > NUM_MEM_COUNTERS)
        {
            return ENOMEM;
        }

        if (memCounterCount.compare_exchange_weak(base, base + count, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            break;
        }
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        memCounters[base + i].name = names[i];
    }

    group->startIndex = base;
    group->count = count;
    return 0;
}

uint32_t Perf::getMemCounters(uint32_t index, MemCounter * counterArray, uint32_t counterArraySize) noexcept
{
    uint32_t totalCount = memCounterCount.load(std::memory_order_relaxed);
    if (index >= totalCount)
    {
        return 0;
    }

    uint32_t count = std::min(counterArraySize, totalCount - index);
    for (uint32_t i = 0; i < count; ++i)
    {
        counterArray[i].reset();
    }

    uint32_t processorCount = getProcessorCount();
    for (uint32_t cpu = 0; cpu < processorCount; ++cpu)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            counterArray[i].accumulate(processorState[cpu].memCounters[index + i]);
        }
    }

    return count;
}

} // namespace silk
