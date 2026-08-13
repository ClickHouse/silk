#pragma once

#include <cstdint>

#include <sched.h>

namespace silk
{

struct CpuTopology
{
    uint32_t packageId = UINT32_MAX;
    uint32_t coreId = UINT32_MAX;
    uint32_t numaNodeId = UINT32_MAX;
};

/**
 * Fill @p topologies[0..processorCount) with topology data read from sysfs.
 * Fields remain UINT32_MAX for CPUs whose sysfs entries are absent (e.g. containers).
 */
void readCpuTopologies(CpuTopology * topologies, uint32_t processorCount) noexcept;

/**
 * Return the steal cost in TSC cycles between two CPUs with the given topologies.
 * HT sibling ~1 us, same NUMA ~50 us, cross-NUMA ~500 us.
 */
uint64_t topologyCostCycles(const CpuTopology & first, const CpuTopology & second) noexcept;

/**
 * Test whether @p cpu is in a Linux cpulist string. Accepts the kernel grammar
 * "[a[-b[:c[/d]]]](,...)" with optional stride: positions in [a, b] for which
 * (pos - a) % d < c. Defaults are b=a, c=1, d=c. Exposed for unit tests.
 */
bool cpuInCpulist(uint32_t cpu, const char * list) noexcept;

/**
 * Whether @p cpu belongs to the scheduler's active set: it must be in both the
 * affinity mask @p affinityMask (the initializing thread's mask) and @p cpuMask.
 */
static inline bool isCpuActive(uint32_t cpu, const cpu_set_t & affinityMask, const cpu_set_t & cpuMask) noexcept
{
    return CPU_ISSET(cpu, &affinityMask) && CPU_ISSET(cpu, &cpuMask);
}

/**
 * Pin the calling thread to the CPUs in @p cpuSet. The kernel migrates the
 * thread off any excluded CPU before the call returns. Returns 0 or the errno
 * from pthread_setaffinity_np.
 */
int pinThreadToCpus(const cpu_set_t & cpuSet) noexcept;

/**
 * Pin the calling thread to @p cpu. The kernel migrates the thread before the
 * call returns, so on return the thread already runs on @p cpu and
 * getCurrentProcessor observes it. Returns 0 or the errno from
 * pthread_setaffinity_np.
 */
int pinThreadToCpu(uint16_t cpu) noexcept;

} // namespace silk
