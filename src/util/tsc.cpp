#include <silk/util/tsc.h>

#include <silk/util/assert.h>

#include <cstring>
#include <format>

#if defined(__x86_64__)
#    include <cpuid.h>
#endif

namespace silk
{

#if defined(__x86_64__)

// CPUID leaf 0x80000007 EDX bit 8 advertises the Invariant TSC feature:
// TSC runs at a constant rate across frequency transitions and remains synchronized
// across cores. Silk reads TSC on one CPU and compares the result on another
// (sleep deadlines, work-stealing budgets) -- if TSC is not invariant those
// comparisons are unsound. Older CPUs without this bit are not supported.
static bool hasInvariantTsc() noexcept
{
    uint32_t eax, ebx, ecx, edx;
    if (__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx) == 0 || eax < 0x80000007)
    {
        return false;
    }
    __cpuid(0x80000007, eax, ebx, ecx, edx);
    return (edx & (1u << 8)) != 0;
}

static uint64_t getTscFrequencyCpuid() noexcept
{
    uint32_t eax, ebx, ecx, edx;
    __cpuid(1, eax, ebx, ecx, edx);

    // Check for hypervisor presence (leaf 1 ECX bit 31).
    if (ecx & (1u << 31))
    {
        uint32_t hv_max;
        __cpuid(0x40000000, hv_max, ebx, ecx, edx);

        char vendor[13];
        memcpy(vendor + 0, &ebx, 4);
        memcpy(vendor + 4, &ecx, 4);
        memcpy(vendor + 8, &edx, 4);
        vendor[12] = '\0';

        if (strcmp(vendor, "KVMKVMKVM") == 0)
        {
            // KVM (AWS Nitro, GCP): leaf 0x40000010 EAX = TSC frequency in kHz.
            if (hv_max >= 0x40000010)
            {
                __cpuid(0x40000010, eax, ebx, ecx, edx);
                if (eax != 0)
                {
                    return static_cast<uint64_t>(eax) * 1000; // kHz -> Hz
                }
            }
        }
        // Hyper-V (Azure) does not expose a TSC frequency CPUID leaf;
        // fall through to the standard leaf 0x15 path below.
    }

    // Bare metal and Hyper-V (Azure): leaf 0x15 TSC/crystal clock ratio.
    //   EAX = denominator, EBX = numerator, ECX = crystal Hz (0 if not enumerated).
    uint32_t max_leaf;
    __get_cpuid(0, &max_leaf, &ebx, &ecx, &edx);
    if (max_leaf < 0x15)
    {
        return 0;
    }

    uint32_t tsc_denom, tsc_numer, crystal_hz;
    __cpuid_count(0x15, 0, tsc_denom, tsc_numer, crystal_hz, edx);
    if (tsc_denom == 0 || tsc_numer == 0)
    {
        return 0;
    }

    if (crystal_hz != 0)
    {
        return static_cast<uint64_t>(crystal_hz) * tsc_numer / tsc_denom; // Hz
    }

    // Crystal Hz not enumerated: leaf 0x16 EAX[15:0] gives the processor base
    // frequency in MHz, which equals the TSC frequency for an invariant TSC.
    if (max_leaf >= 0x16)
    {
        uint32_t base_mhz;
        __cpuid_count(0x16, 0, base_mhz, ebx, ecx, edx);
        base_mhz &= 0xffff;
        if (base_mhz != 0)
        {
            return static_cast<uint64_t>(base_mhz) * 1'000'000; // MHz -> Hz
        }
    }

    return 0;
}
#endif // __x86_64__

void Tsc::initialize() noexcept
{
    if (frequency)
    {
        // Skip the second initialization.
        return;
    }

#if defined(__x86_64__)
    SILK_ASSERT(hasInvariantTsc(), "CPU does not advertise Invariant TSC; silk requires a stable cross-core counter");
    frequency = getTscFrequencyCpuid();
#elif defined(__aarch64__)
    // ARMv8 cntvct_el0 is anchored to a system counter that is invariant by
    // architecture, so no equivalent capability check is required.
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
#else
#    error Unsupported platform
#endif

    SILK_ASSERT(frequency != 0, "failed to determine TSC frequency");
    nsPerCycleFp = (1ULL << SHIFT) * 1'000'000'000 / frequency;
    cyclesPerNsFp = frequency * (1ULL << SHIFT) / 1'000'000'000;
}

} // namespace silk
