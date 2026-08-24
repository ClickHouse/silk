#pragma once

/**
 * Print the "scheduler_latency" JSON section, aggregating per-CPU profiler
 * histograms by event kind and fiber category.
 */
void printSchedulerLatency() noexcept;

/**
 * Print the "counters" JSON section: scheduler-wide simple counters.
 * Outputs no trailing comma; intended as the last field of the JSON object.
 */
void printCounters() noexcept;
