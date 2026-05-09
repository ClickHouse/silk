#include "fibers/profiler.h"

#include <silk/fibers/fiber.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace silk
{

static ProfileEvent makeEvent(ProfileEventKind kind, uint8_t category, uint64_t durationCycles) noexcept
{
    ProfileEvent event;
    event.duration = durationCycles;
    event.category = category;
    event.kind = static_cast<uint8_t>(kind);
    return event;
}

// Single enqueue/aggregate routes the event into the correct (kind, category) bucket.
TEST(Profiler, enqueueAggregate)
{
    auto profiler = std::make_unique<Profiler>();
    EXPECT_TRUE(profiler->enqueue(makeEvent(ProfileEventKind::READY_WAIT, 7, 1024)));
    profiler->aggregate();

    EXPECT_EQ(profiler->histogram(ProfileEventKind::READY_WAIT, 7).count(), 1u);
    EXPECT_EQ(profiler->histogram(ProfileEventKind::READY_WAIT, 0).count(), 0u);
    EXPECT_EQ(profiler->histogram(ProfileEventKind::FIBER_RUN, 7).count(), 0u);
}

// Each ProfileEventKind routes independently.
TEST(Profiler, allKinds)
{
    auto profiler = std::make_unique<Profiler>();
    for (uint32_t k = 0; k < static_cast<uint32_t>(ProfileEventKind::MAX); ++k)
    {
        auto kind = static_cast<ProfileEventKind>(k);
        EXPECT_TRUE(profiler->enqueue(makeEvent(kind, 0, 1024)));
    }
    profiler->aggregate();

    for (uint32_t k = 0; k < static_cast<uint32_t>(ProfileEventKind::MAX); ++k)
    {
        auto kind = static_cast<ProfileEventKind>(k);
        EXPECT_EQ(profiler->histogram(kind, 0).count(), 1u);
    }
}

// Filling the ring without aggregating causes enqueue to return false.
TEST(Profiler, enqueueRingFull)
{
    auto profiler = std::make_unique<Profiler>();
    constexpr uint32_t RING_CAPACITY = 4096;

    for (uint32_t i = 0; i < RING_CAPACITY; ++i)
    {
        EXPECT_TRUE(profiler->enqueue(makeEvent(ProfileEventKind::READY_WAIT, 0, 1024))) << "iteration " << i;
    }
    EXPECT_FALSE(profiler->enqueue(makeEvent(ProfileEventKind::READY_WAIT, 0, 1024)));
    EXPECT_FALSE(profiler->enqueue(makeEvent(ProfileEventKind::READY_WAIT, 0, 1024)));
}

// After aggregate drains the ring, new enqueues succeed again.
TEST(Profiler, aggregateFreesRing)
{
    auto profiler = std::make_unique<Profiler>();
    constexpr uint32_t RING_CAPACITY = 4096;

    for (uint32_t i = 0; i < RING_CAPACITY; ++i)
    {
        EXPECT_TRUE(profiler->enqueue(makeEvent(ProfileEventKind::IO_WAIT, 1, 2048)));
    }
    EXPECT_FALSE(profiler->enqueue(makeEvent(ProfileEventKind::IO_WAIT, 1, 2048)));

    profiler->aggregate();

    EXPECT_EQ(profiler->histogram(ProfileEventKind::IO_WAIT, 1).count(), RING_CAPACITY);

    EXPECT_TRUE(profiler->enqueue(makeEvent(ProfileEventKind::IO_WAIT, 1, 2048)));
    profiler->aggregate();
    EXPECT_EQ(profiler->histogram(ProfileEventKind::IO_WAIT, 1).count(), RING_CAPACITY + 1);
}

// Many enqueue/aggregate cycles keep accumulating into the same histogram.
TEST(Profiler, repeatedAggregateAccumulates)
{
    auto profiler = std::make_unique<Profiler>();
    constexpr uint32_t TOTAL = 10000;

    uint32_t enqueued = 0;
    while (enqueued < TOTAL)
    {
        if (profiler->enqueue(makeEvent(ProfileEventKind::FIBER_RUN, 3, 512)))
        {
            ++enqueued;
        }
        else
        {
            profiler->aggregate();
        }
    }
    profiler->aggregate();

    EXPECT_EQ(profiler->histogram(ProfileEventKind::FIBER_RUN, 3).count(), TOTAL);
}

} // namespace silk
