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

#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[1;31m"
#define COLOR_YELLOW "\033[0;33m"
#define COLOR_GREEN "\033[0;32m"

namespace silk
{

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
    std::fprintf(stderr, "#%d  %#018lx in " COLOR_YELLOW, ctx->frame++, static_cast<unsigned long>(pc));

    int status = 0;
    char * demangled = function ? abi::__cxa_demangle(function, nullptr, nullptr, &status) : nullptr;
    if (demangled && status == 0)
    {
        const char * paren = std::strrchr(demangled, '(');
        if (paren)
        {
            std::fwrite(demangled, 1, static_cast<size_t>(paren - demangled), stderr);
            std::fprintf(stderr, COLOR_RESET " %s", paren);
        }
        else
        {
            std::fprintf(stderr, "%s" COLOR_RESET " ()", demangled);
        }
    }
    else
    {
        std::fprintf(stderr, "%s" COLOR_RESET " ()", function ? function : "??");
    }

    if (filename)
    {
        std::fprintf(stderr, " at " COLOR_GREEN "%s" COLOR_RESET ":%d", filename, lineno);
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

void assertFail(const char * file, int line, const char * message, const char * fmt, ...) noexcept
{
    ::flockfile(stderr);

    std::fprintf(stderr, COLOR_RED "%s:%d ", file, line);
    if (message)
    {
        std::fputs(message, stderr);
    }
    if (fmt)
    {
        if (message)
        {
            std::fputs(" -- ", stderr);
        }
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    std::fputs(COLOR_RESET "\n", stderr);

#if defined(SILK_USE_LIBBACKTRACE)
    Context ctx;
    backtrace_full(btState, 0, btCallback, btErrorCallback, &ctx);
#endif

    ::funlockfile(stderr);

    std::fflush(stderr);
    std::abort();
}

} // namespace silk

#undef COLOR_RESET
#undef COLOR_RED
#undef COLOR_YELLOW
#undef COLOR_GREEN
