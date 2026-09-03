#include "parse.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

uint64_t parseSize(const std::string & str)
{
    size_t pos;
    uint64_t n = std::stoull(str, &pos);
    std::string_view suffix = std::string_view(str).substr(pos);
    uint64_t multiplier = 1;

    if (suffix == "k" || suffix == "K")
    {
        multiplier = 1024ULL;
    }
    else if (suffix == "m" || suffix == "M")
    {
        multiplier = 1024ULL * 1024;
    }
    else if (suffix == "g" || suffix == "G")
    {
        multiplier = 1024ULL * 1024 * 1024;
    }
    else if (!suffix.empty())
    {
        throw std::invalid_argument("unknown size suffix: " + std::string(suffix));
    }

    if (n > std::numeric_limits<uint64_t>::max() / multiplier)
    {
        throw std::out_of_range("size is too large: " + str);
    }

    return n * multiplier;
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
