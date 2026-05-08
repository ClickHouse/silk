#include <silk/util/assert.h>

#include <silk/util/platform.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>

#if defined(SILK_USE_LIBBACKTRACE)
#    include <cstring>
#    include <format>
#    include <string_view>

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
    std::ostringstream * out;
    int frame = 0;
};

static int btCallback(void * data, uintptr_t pc, const char * filename, int lineno, const char * function) noexcept
{
    Context * ctx = static_cast<Context *>(data);
    (*ctx->out) << "#" << ctx->frame++ << "  " << std::format("{:#018x}", pc) << " in " << COLOR_YELLOW;

    int status = 0;
    char * demangled = function ? abi::__cxa_demangle(function, nullptr, nullptr, &status) : nullptr;
    if (demangled && status == 0)
    {
        const char * paren = std::strchr(demangled, '(');
        if (paren)
        {
            (*ctx->out) << std::string_view(demangled, paren - demangled);
            (*ctx->out) << COLOR_RESET << " " << paren;
        }
        else
        {
            (*ctx->out) << demangled << COLOR_RESET << " ()";
        }
    }
    else
    {
        (*ctx->out) << (function ? function : "??") << COLOR_RESET << " ()";
    }

    if (filename)
    {
        (*ctx->out) << " at " << COLOR_GREEN << filename << COLOR_RESET << ":" << lineno;
    }
    (*ctx->out) << "\n";

    std::free(demangled);
    return 0;
}

static void btErrorCallback(void * data, const char * msg, int err) noexcept
{
    Context * ctx = static_cast<Context *>(data);
    (*ctx->out) << "  backtrace error " << err << ": " << msg << "\n";
}

#endif // SILK_USE_LIBBACKTRACE

void assertFail(const char * message, const char * file, int line, const char * details) noexcept
{
    std::ostringstream out;
    out << COLOR_RED << file << ":" << line << " " << message;
    if (details)
    {
        out << " -- " << details;
    }
    out << COLOR_RESET << "\n";

#if defined(SILK_USE_LIBBACKTRACE)
    Context ctx{&out, 0};
    backtrace_full(btState, 0, btCallback, btErrorCallback, &ctx);
#endif

    std::fputs(out.str().c_str(), stderr);
    std::fflush(stderr);
    std::abort();
}

} // namespace silk
