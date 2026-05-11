#pragma once

#include <atomic>

namespace silk
{

// Log severity, ordered low-to-high. A record is emitted when its level is
// >= the logger's configured minimum (default INFO; change via Logger::setLevel).
enum class LogLevel
{
    DEBUG,
    INFO,
    WARN,
    ERROR,
};

/**
 * Minimalistic thread-safe logger with a global log level filter.
 *
 * Use the SILK_DEBUG / SILK_INFO / SILK_WARN / SILK_ERROR macros for structured
 * logging with automatic file and line capture. The level check is inlined at
 * the call site so disabled levels have no formatting overhead.
 */
class Logger
{
public:
    /** Set the minimum level for messages to be emitted. */
    static void setLevel(LogLevel level) noexcept { currentLevel.store(level, std::memory_order_relaxed); }

    /** Return true if messages at the given level would be emitted. */
    static bool isEnabled(LogLevel level) noexcept { return level >= currentLevel.load(std::memory_order_relaxed); }

    /** Emit a pre-formatted message verbatim (no printf parsing). */
    static void log(LogLevel level, const char * file, int line, const char * message) noexcept;

    /** Emit a printf-formatted message. */
    static void logf(LogLevel level, const char * file, int line, const char * fmt, ...) noexcept __attribute__((format(printf, 4, 5)));

private:
    inline static std::atomic<LogLevel> currentLevel{LogLevel::INFO};
};

} // namespace silk

// Emit a log record at the given level. Dispatches to Logger::log for plain
// strings (SILK_INFO("text")) and to Logger::logf for printf-style calls
// (SILK_INFO("foo %d", x)) -- the latter is the only path that pays vsnprintf.
// The format string is checked at every call site (-Wformat); formatting only
// runs when the level is enabled, so a disabled-level call site is one relaxed
// atomic load.
#define SILK_LOG(level, fmt, ...) \
    do \
    { \
        if (silk::Logger::isEnabled(level)) \
        { \
            silk::Logger::log##__VA_OPT__(f)(level, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__); \
        } \
    } while (0)

#define SILK_DEBUG(fmt, ...) SILK_LOG(silk::LogLevel::DEBUG, fmt __VA_OPT__(, ) __VA_ARGS__)
#define SILK_INFO(fmt, ...) SILK_LOG(silk::LogLevel::INFO, fmt __VA_OPT__(, ) __VA_ARGS__)
#define SILK_WARN(fmt, ...) SILK_LOG(silk::LogLevel::WARN, fmt __VA_OPT__(, ) __VA_ARGS__)
#define SILK_ERROR(fmt, ...) SILK_LOG(silk::LogLevel::ERROR, fmt __VA_OPT__(, ) __VA_ARGS__)
