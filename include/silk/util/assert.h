#pragma once

namespace silk
{

#if !defined(DISABLE_ASSERTIONS)
static constexpr bool ReleaseAssertionsEnabled = true;
#else
static constexpr bool ReleaseAssertionsEnabled = false;
#endif

#if !defined(NDEBUG) && !defined(DISABLE_ASSERTIONS)
static constexpr bool DebugAssertionsEnabled = true;
#else
static constexpr bool DebugAssertionsEnabled = false;
#endif

/**
 * Print a symbolized stack trace, the error message, optional printf-formatted
 * details, and abort. `fmt` is a printf-style format string; pass nullptr (or
 * omit) to skip the details line.
 */
[[noreturn]] void assertFail(const char * file, int line, const char * message, const char * fmt = nullptr, ...) noexcept
    __attribute__((format(printf, 4, 5)));

} // namespace silk

// Unconditional abort.
#define SILK_FAIL(...) silk::assertFail(__FILE__, __LINE__, nullptr, __VA_ARGS__)

// Assertion active in all builds unless DISABLE_ASSERTIONS is defined.
// Use for invariants that must hold in release -- hard internal contract violations.
#define SILK_ASSERT(condition, ...) \
    do \
    { \
        if (silk::ReleaseAssertionsEnabled && !(condition)) [[unlikely]] \
        { \
            silk::assertFail(__FILE__, __LINE__, "assertion failed: " #condition __VA_OPT__(, ) __VA_ARGS__); \
        } \
    } while (0)

// Assertion active only in debug builds (DEBUG defined).
// Use for expensive checks or invariants only relevant during development.
#define SILK_ASSERT_DEBUG(condition, ...) \
    do \
    { \
        if (silk::DebugAssertionsEnabled && !(condition)) [[unlikely]] \
        { \
            silk::assertFail(__FILE__, __LINE__, "assertion failed: " #condition __VA_OPT__(, ) __VA_ARGS__); \
        } \
    } while (0)
