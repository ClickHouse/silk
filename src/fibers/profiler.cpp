#include "profiler.h"

#include <silk/util/tsc.h>

namespace silk
{

bool Profiler::enqueue(ProfileEvent event) noexcept
{
    uint64_t write = writeIndex.load(std::memory_order_relaxed);
    uint64_t read = readIndex.load(std::memory_order_acquire);
    if (write - read < RING_CAPACITY)
    {
        events[write & (RING_CAPACITY - 1)].store(event.raw, std::memory_order_relaxed);
        writeIndex.store(write + 1, std::memory_order_release);
        return true;
    }
    return false;
}

bool Profiler::dequeue(ProfileEvent * event) noexcept
{
    uint64_t read = readIndex.load(std::memory_order_relaxed);
    uint64_t write = writeIndex.load(std::memory_order_acquire);
    if (read < write)
    {
        event->raw = events[read & (RING_CAPACITY - 1)].load(std::memory_order_relaxed);
        readIndex.store(read + 1, std::memory_order_release);
        return true;
    }
    return false;
}

void Profiler::aggregate() noexcept
{
    ProfileEvent event;
    while (dequeue(&event))
    {
        if (event.kind < NUM_KINDS)
        {
            uint64_t ns = Tsc::cyclesToNanoseconds(event.duration);
            histograms[event.kind][event.category].record(ns);
        }
    }
}

} // namespace silk
