#include <silk/util/tsc.h>

#include <silk/util/platform.h>

#include <gtest/gtest.h>

namespace silk
{

TEST(Tsc, FrequencyIsReasonable)
{
    uint64_t freq = Tsc::getFrequency();
#if defined(__aarch64__)
    // On ARM getFrequency() returns cntfrq_el0, the architectural system counter
    // frequency, which is unrelated to the CPU clock -- e.g. ~122 MHz on AWS
    // Graviton, but other parts run it faster (1.05 GHz observed). Bound it
    // loosely to catch a garbage reading.
    EXPECT_GE(freq, 1'000'000ULL); // >= 1 MHz
    EXPECT_LE(freq, 4'000'000'000ULL); // <= 4 GHz
#else
    // On x86 the TSC is anchored to the CPU clock.
    EXPECT_GE(freq, 1'000'000'000ULL); // >= 1 GHz
    EXPECT_LE(freq, 10'000'000'000ULL); // <= 10 GHz
#endif
}

TEST(Tsc, CyclesAdvance)
{
    uint64_t a = Tsc::getCycles();
    // A low-frequency counter (e.g. ARM cntvct_el0 at ~122 MHz, ~8 ns/tick) can
    // return the same value for two back-to-back reads that fall in one tick, so
    // spin until it advances. It must advance and never run backwards.
    uint64_t b = a;
    for (int i = 0; i < 100'000'000 && b == a; ++i)
    {
        b = Tsc::getCycles();
        ASSERT_GE(b, a); // never moves backwards
    }
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
