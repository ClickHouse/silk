#include <silk/util/assert.h>

#include <silk/util/platform.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(SILK_USE_LIBBACKTRACE)
#    include <backtrace.h>
#    include <cxxabi.h>
#endif // SILK_USE_LIBBACKTRACE

namespace silk
{

static constexpr const char * COLOR_RESET = "\033[0m";
static constexpr const char * COLOR_RED = "\033[1;31m";
static constexpr const char * COLOR_YELLOW = "\033[0;33m";
static constexpr const char * COLOR_GREEN = "\033[0;32m";

#if defined(SILK_USE_LIBBACKTRACE)

static void btCreateErrorCallback(void * data, const char * msg, int err) noexcept
{
    SILK_UNUSED(data);
    std::fprintf(stderr, "backtrace_create_state error %d: %s\n", err, msg);
}

static backtrace_state * btState = backtrace_create_state(nullptr, 1, btCreateErrorCallback, nullptr);

struct Context
{
    int frame = 0;
};

static int btCallback(void * data, uintptr_t pc, const char * filename, int lineno, const char * function) noexcept
{
    Context * ctx = static_cast<Context *>(data);
    std::fprintf(stderr, "#%d  %#018lx in %s", ctx->frame++, static_cast<unsigned long>(pc), COLOR_YELLOW);

    int status = 0;
    char * demangled = function ? abi::__cxa_demangle(function, nullptr, nullptr, &status) : nullptr;
    if (demangled && status == 0)
    {
        const char * paren = std::strchr(demangled, '(');
        if (paren)
        {
            std::fwrite(demangled, 1, static_cast<size_t>(paren - demangled), stderr);
            std::fprintf(stderr, "%s %s", COLOR_RESET, paren);
        }
        else
        {
            std::fprintf(stderr, "%s%s ()", demangled, COLOR_RESET);
        }
    }
    else
    {
        std::fprintf(stderr, "%s%s ()", function ? function : "??", COLOR_RESET);
    }

    if (filename)
    {
        std::fprintf(stderr, " at %s%s%s:%d", COLOR_GREEN, filename, COLOR_RESET, lineno);
    }
    std::fputc('\n', stderr);

    std::free(demangled);
    return 0;
}

static void btErrorCallback(void * data, const char * msg, int err) noexcept
{
    SILK_UNUSED(data);
    std::fprintf(stderr, "  backtrace error %d: %s\n", err, msg);
}

#endif // SILK_USE_LIBBACKTRACE

void assertFail(const char * message, const char * file, int line, const char * fmt, ...) noexcept
{
    ::flockfile(stderr);

    std::fprintf(stderr, "%s%s:%d %s", COLOR_RED, file, line, message);
    if (fmt)
    {
        std::fputs(" -- ", stderr);
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    std::fprintf(stderr, "%s\n", COLOR_RESET);

#if defined(SILK_USE_LIBBACKTRACE)
    Context ctx;
    backtrace_full(btState, 0, btCallback, btErrorCallback, &ctx);
#endif

    ::funlockfile(stderr);

    std::fflush(stderr);
    std::abort();
}

} // namespace silk
