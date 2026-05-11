#include <silk/util/logger.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace silk
{

static constexpr const char * LEVEL_NAMES[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};

void Logger::log(LogLevel level, const char * file, int line, const char * message) noexcept
{
    timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);

    tm t;
    ::gmtime_r(&ts.tv_sec, &t);

    std::fprintf(
        stderr,
        "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] %s:%d: %s\n",
        t.tm_year + 1900,
        t.tm_mon + 1,
        t.tm_mday,
        t.tm_hour,
        t.tm_min,
        t.tm_sec,
        ts.tv_nsec / 1'000'000,
        LEVEL_NAMES[static_cast<int>(level)],
        file,
        line,
        message);
}

void Logger::logf(LogLevel level, const char * file, int line, const char * fmt, ...) noexcept
{
    // Format into a small stack buffer, falling back to malloc on overflow,
    // then forward to the pre-formatted log for the actual emission.

    char stackBody[256];
    char * body = stackBody;
    char * heapBody = nullptr;

    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(stackBody, sizeof(stackBody), fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        n = 0;
    }

    if (n >= static_cast<int>(sizeof(stackBody)))
    {
        heapBody = static_cast<char *>(std::malloc(static_cast<size_t>(n) + 1));
        if (heapBody)
        {
            std::vsnprintf(heapBody, static_cast<size_t>(n) + 1, fmt, ap2);
            body = heapBody;
        }
    }
    va_end(ap2);

    log(level, file, line, body);

    std::free(heapBody);
}

} // namespace silk
