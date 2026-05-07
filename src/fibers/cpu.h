#pragma once

#include <cstdint>

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

} // namespace silk
