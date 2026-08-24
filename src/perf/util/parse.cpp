#include "parse.h"

#include <stdexcept>
#include <string>

uint64_t parseSize(const std::string & str)
{
    uint64_t n = std::stoull(str);
    char suffix = str.empty() ? '\0' : str.back();
    if (suffix == 'k' || suffix == 'K')
    {
        return n * 1024ULL;
    }
    if (suffix == 'm' || suffix == 'M')
    {
        return n * 1024ULL * 1024;
    }
    if (suffix == 'g' || suffix == 'G')
    {
        return n * 1024ULL * 1024 * 1024;
    }
    return n;
}

uint64_t parseDuration(const std::string & str)
{
    size_t pos;
    uint64_t n = std::stoull(str, &pos);
    std::string suffix = str.substr(pos);
    if (suffix == "ns")
    {
        return n;
    }
    if (suffix.empty())
    {
        return n * 1'000'000'000ULL;
    }
    if (suffix == "us")
    {
        return n * 1'000ULL;
    }
    if (suffix == "ms")
    {
        return n * 1'000'000ULL;
    }
    if (suffix == "s")
    {
        return n * 1'000'000'000ULL;
    }
    if (suffix == "m")
    {
        return n * 60'000'000'000ULL;
    }
    throw std::invalid_argument("unknown duration suffix: " + suffix);
}

std::string formatDuration(uint64_t ns)
{
    if (ns % 1'000'000'000ULL == 0)
    {
        return std::to_string(ns / 1'000'000'000ULL) + "s";
    }
    if (ns % 1'000'000ULL == 0)
    {
        return std::to_string(ns / 1'000'000ULL) + "ms";
    }
    if (ns % 1'000ULL == 0)
    {
        return std::to_string(ns / 1'000ULL) + "us";
    }
    return std::to_string(ns) + "ns";
}
