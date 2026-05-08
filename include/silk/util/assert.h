#pragma once

namespace silk
{

#if !defined(DISABLE_ASSERTIONS)
static constexpr bool ReleaseAssertionsEnabled = true;
#else
static constexpr bool ReleaseAssertionsEnabled = false;
#endif

#if defined(DEBUG) && !defined(DISABLE_ASSERTIONS)
static constexpr bool DebugAssertionsEnabled = true;
#else
static constexpr bool DebugAssertionsEnabled = false;
#endif

/**
 * Print a symbolized stack trace, the error message, and optional details, then abort.
 */
[[noreturn]] void assertFail(const char * message, const char * file, int line, const char * details = nullptr) noexcept;

} // namespace silk

// Assertion active in all builds unless DISABLE_ASSERTIONS is defined.
// Use for invariants that must hold in release -- hard internal contract violations.
#define SILK_ASSERT(condition, ...) \
    do \
    { \
        if (silk::ReleaseAssertionsEnabled && !(condition)) [[unlikely]] \
        { \
            silk::assertFail("assertion failed: " #condition, __FILE__, __LINE__ __VA_OPT__(, std::format(__VA_ARGS__).c_str())); \
        } \
    } while (0)

// Assertion active only in debug builds (DEBUG defined).
// Use for expensive checks or invariants only relevant during development.
#define SILK_ASSERT_DEBUG(condition, ...) \
    do \
    { \
        if (silk::DebugAssertionsEnabled && !(condition)) [[unlikely]] \
        { \
            silk::assertFail("assertion failed: " #condition, __FILE__, __LINE__ __VA_OPT__(, std::format(__VA_ARGS__).c_str())); \
        } \
    } while (0)
