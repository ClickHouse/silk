#include <silk/util/logger.h>

#include <cstdio>
#include <ctime>
#include <string_view>

namespace silk
{

static constexpr std::string_view LEVEL_NAMES[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};

void Logger::setLevel(LogLevel level) noexcept
{
    currentLevel.store(level, std::memory_order_relaxed);
}

void Logger::log(LogLevel level, std::string_view file, int line, std::string_view message) noexcept
{
    timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);

    tm t;
    ::gmtime_r(&ts.tv_sec, &t);

    std::fprintf(
        stderr,
        "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] %.*s:%d: %.*s\n",
        t.tm_year + 1900,
        t.tm_mon + 1,
        t.tm_mday,
        t.tm_hour,
        t.tm_min,
        t.tm_sec,
        ts.tv_nsec / 1'000'000,
        LEVEL_NAMES[static_cast<int>(level)].data(),
        static_cast<int>(file.size()),
        file.data(),
        line,
        static_cast<int>(message.size()),
        message.data());
}

} // namespace silk
