#pragma once

#include <cstdint>

namespace silk
{

/**
 * TSC frequency query and cycle/nanosecond conversion.
 *
 * Frequency is queried once via CPUID at initialization and cached.
 * Conversions use fixed-point multiply+shift - no division on the hot path.
 */
class Tsc
{
public:
    /**
     * Read the current value of the CPU timestamp counter.
     *
     * Note: no serializing barrier is issued. The CPU may reorder this read
     * against surrounding instructions. For precise interval measurement use
     * an lfence before the start reading and an lfence+rdtscp (or mfence) for
     * the end reading. For deadline calculation (rdtsc + offset) reordering is
     * not a concern.
     */
    static uint64_t getCycles() noexcept
    {
#if defined(__x86_64__)
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
        uint64_t val;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
        return val;
#else
#    error Unsupported platform
#endif
    }

    /**
     * Query TSC frequency, populate the fixed-point conversion factors, and
     * assert that the running CPU advertises an invariant TSC. Idempotent.
     */
    static void initialize() noexcept;

    /** Return the TSC frequency in Hz. */
    static uint64_t getFrequency() noexcept { return frequency; }

    /** Convert TSC cycles to nanoseconds. */
    static uint64_t cyclesToNanoseconds(uint64_t cycles) noexcept { return (cycles * nsPerCycleFp) >> SHIFT; }

    /** Convert nanoseconds to TSC cycles. */
    static uint64_t nanosecondsToCycles(uint64_t ns) noexcept { return (ns * cyclesPerNsFp) >> SHIFT; }

private:
    static constexpr uint32_t SHIFT = 20;

    inline static uint64_t frequency = 0; ///< Hz
    inline static uint64_t nsPerCycleFp = 0; ///< (1 << SHIFT) * 1'000'000'000 / frequency
    inline static uint64_t cyclesPerNsFp = 0; ///< frequency * (1 << SHIFT) / 1'000'000'000
};

} // namespace silk
