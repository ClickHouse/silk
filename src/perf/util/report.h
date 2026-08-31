#pragma once

#include "pmc.h"

#include <cstdint>

/**
 * Print the "scheduler_latency" JSON section, aggregating per-CPU profiler
 * histograms by event kind and fiber category.
 */
void printSchedulerLatency() noexcept;

/**
 * Print the "pmc" JSON section: hardware/software counter totals over the
 * counting window plus per-IO ratios (windowIos is the number of IOs
 * completed inside that window). Outputs no trailing comma.
 */
void printPmc(const Pmc::Counts & counts, uint64_t windowIos) noexcept;

/**
 * Print the "counters" JSON section: scheduler-wide simple counters.
 * Outputs no trailing comma; intended as the last field of the JSON object.
 */
void printCounters() noexcept;
