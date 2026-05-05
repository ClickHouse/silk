#include <silk/util/tsc.h>

#include <silk/util/platform.h>

#include <gtest/gtest.h>

namespace silk
{

TEST(Tsc, FrequencyIsReasonable)
{
    uint64_t freq = Tsc::getFrequency();
    EXPECT_GE(freq, 1'000'000'000ULL); // >= 1 GHz
    EXPECT_LE(freq, 10'000'000'000ULL); // <= 10 GHz
}

TEST(Tsc, CyclesAdvance)
{
    uint64_t a = Tsc::getCycles();
    uint64_t b = Tsc::getCycles();
    EXPECT_GT(b, a);
}

TEST(Tsc, CyclesToNanosecondsRoundTrip)
{
    constexpr uint64_t ns = 1'000'000; // 1 ms
    uint64_t cycles = Tsc::nanosecondsToCycles(ns);
    uint64_t result = Tsc::cyclesToNanoseconds(cycles);
    // Allow 0.2% error from fixed-point rounding
    EXPECT_NEAR(static_cast<double>(result), static_cast<double>(ns), ns * 0.002);
}

TEST(Tsc, NanosecondsMatchWallClock)
{
    uint64_t wall0 = getTimeNanoseconds();
    uint64_t c0 = Tsc::getCycles();

    // Spin for ~1 ms
    for (;;)
    {
        uint64_t wall_ns = getTimeNanoseconds() - wall0;
        if (wall_ns >= 1'000'000)
        {
            uint64_t tsc_ns = Tsc::cyclesToNanoseconds(Tsc::getCycles() - c0);
            // Allow 1% deviation
            EXPECT_NEAR(static_cast<double>(tsc_ns), static_cast<double>(wall_ns), wall_ns * 0.01);
            break;
        }
    }
}

} // namespace silk
