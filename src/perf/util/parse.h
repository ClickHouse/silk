#pragma once

#include <cstdint>
#include <string>

/**
 * Parse a size string with optional k/m/g suffix (case-insensitive) into bytes.
 * Examples: "4k" -> 4096, "1g" -> 1073741824, "512" -> 512.
 */
uint64_t parseSize(const std::string & str);

/**
 * Parse a duration string with an optional unit suffix into nanoseconds.
 * Supported units: ns, us, ms, s, m. No suffix is treated as seconds.
 * Examples: "10" -> 10000000000, "100us" -> 100000, "1ms" -> 1000000.
 */
uint64_t parseDuration(const std::string & str);

/**
 * Format a nanosecond duration as a string using the largest exact unit.
 * Examples: 10000000000 -> "10s", 1500000 -> "1500us", 500 -> "500ns".
 */
std::string formatDuration(uint64_t ns);
