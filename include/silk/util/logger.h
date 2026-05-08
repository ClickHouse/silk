#pragma once

#include <atomic>
#include <format>
#include <string_view>

namespace silk
{

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
    static void setLevel(LogLevel level) noexcept;

    /** Return true if messages at the given level would be emitted. */
    static bool isEnabled(LogLevel level) noexcept { return level >= currentLevel.load(std::memory_order_relaxed); }

    /** Emit a pre-formatted message. */
    static void log(LogLevel level, std::string_view file, int line, std::string_view message) noexcept;

    /** Format and emit a message. */
    template <typename... Args>
    static void log(LogLevel level, std::string_view file, int line, std::format_string<Args...> fmt, Args &&... args) noexcept
    {
        log(level, file, line, std::format(fmt, std::forward<Args>(args)...));
    }

private:
    inline static std::atomic<LogLevel> currentLevel{LogLevel::INFO};
};

} // namespace silk

// clang-format off
#define SILK_DEBUG(...) do { if (silk::Logger::isEnabled(silk::LogLevel::DEBUG)) silk::Logger::log(silk::LogLevel::DEBUG, __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define SILK_INFO(...)  do { if (silk::Logger::isEnabled(silk::LogLevel::INFO))  silk::Logger::log(silk::LogLevel::INFO,  __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define SILK_WARN(...)  do { if (silk::Logger::isEnabled(silk::LogLevel::WARN))  silk::Logger::log(silk::LogLevel::WARN,  __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define SILK_ERROR(...) do { if (silk::Logger::isEnabled(silk::LogLevel::ERROR)) silk::Logger::log(silk::LogLevel::ERROR, __FILE__, __LINE__, __VA_ARGS__); } while (0)
// clang-format on
