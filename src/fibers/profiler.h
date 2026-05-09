#pragma once

#include "histogram.h"

#include <silk/fibers/fiber.h>
#include <silk/util/platform.h>

#include <atomic>
#include <cstdint>

namespace silk
{

/**
 * Packed profile event: [kind:8 | category:8 | duration:48] stored as uint64_t.
 *  kind:     ProfileEventKind identifying the measured interval.
 *  category: FiberId::category of the fiber that triggered the event.
 *  duration: elapsed TSC cycles for the measured interval.
 */
union ProfileEvent
{
    struct
    {
        uint64_t duration : 48;
        uint64_t category : 8;
        uint64_t kind : 8;
    };
    uint64_t raw;
};
static_assert(sizeof(ProfileEvent) == 8);

/**
 * Per-CPU profiler: SPSC event ring + per-(kind x category) histograms.
 * The scheduler thread for this CPU is the sole producer. The consumer
 * is serialized by ProcessorState::serviceLoopLock, so dequeue instances
 * linearize through that lock and the ring is safe to treat as SPSC.
 */
class Profiler
{
public:
    /**
     * Enqueue a profile event. Called by the scheduler thread.
     * Returns true on success, false if the ring was full and the event was dropped.
     */
    [[nodiscard]] bool enqueue(ProfileEvent event) noexcept;

    /** Drain all pending events from the ring into the histograms. Called by the service loop. */
    void aggregate() noexcept;

    /** Return the aggregated histogram for the given event kind and fiber category. */
    const Histogram & histogram(ProfileEventKind kind, uint8_t category) const noexcept { return histograms[uint8_t(kind)][category]; }

private:
    //
    // Constants.
    //

    static constexpr uint32_t NUM_KINDS = static_cast<uint32_t>(ProfileEventKind::MAX);
    static constexpr uint32_t NUM_CATEGORIES = 256;

    static constexpr uint32_t RING_CAPACITY = 4096;
    static_assert((RING_CAPACITY & (RING_CAPACITY - 1)) == 0);

    //
    // Helpers.
    //

    bool dequeue(ProfileEvent * event) noexcept;

    //
    // State.
    //

    alignas(CACHELINE_SIZE) std::atomic<uint64_t> writeIndex{};
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> readIndex{};
    std::atomic<uint64_t> events[RING_CAPACITY];
    Histogram histograms[NUM_KINDS][NUM_CATEGORIES];
};

} // namespace silk
