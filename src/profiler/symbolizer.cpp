#include "symbolizer.h"

#include <silk/util/logger.h>
#include <silk/util/platform.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <backtrace.h>
#include <cxxabi.h>

template <typename Callback>
static int parseMapsFile(const char * path, Callback callback) noexcept
{
    FILE * f = ::fopen(path, "r");
    if (!f)
    {
        int r = errno;
        SILK_ERROR("open %s: %s", path, ::strerror(r));
        return r;
    }

    char line[512];
    while (::fgets(line, sizeof(line), f))
    {
        uint64_t start, end, fileOffset;
        char perms[8];
        char mapPath[256] = {};

        // Format: start-end perms offset dev inode [path]
        int n = ::sscanf(line, "%lx-%lx %4s %lx %*s %*u %255s", &start, &end, perms, &fileOffset, mapPath);
        if (n < 4)
        {
            continue;
        }
        if (perms[2] != 'x')
        {
            continue;
        }
        if (mapPath[0] == '\0' || mapPath[0] == '[')
        {
            continue;
        }

        callback(start, end, fileOffset, mapPath);
    }

    ::fclose(f);
    return 0;
}

int Symbolizer::readMappings(uint32_t pid) noexcept
{
    char path[64];
    ::snprintf(path, sizeof(path), "/proc/%u/maps", pid);
    return parseMapsFile(
        path,
        [this](uint64_t start, uint64_t end, uint64_t fileOffset, const char * mapPath)
        { mappings.emplace_back(start, end, fileOffset, mapPath); });
}

int Symbolizer::readSelfMappings() noexcept
{
    return parseMapsFile(
        "/proc/self/maps",
        [this](uint64_t start, uint64_t end, uint64_t fileOffset, const char * mapPath)
        {
            // Record only the first (lowest) executable mapping per path.
            SILK_UNUSED(end);
            selfBase.emplace(mapPath, start - fileOffset);
        });
}

int Symbolizer::readKallsyms() noexcept
{
    FILE * f = ::fopen("/proc/kallsyms", "r");
    if (!f)
    {
        int r = errno;
        SILK_ERROR("open /proc/kallsyms: %s", ::strerror(r));
        return r;
    }

    char line[256];
    while (::fgets(line, sizeof(line), f))
    {
        uint64_t addr;
        char type;
        char name[128] = {};

        if (::sscanf(line, "%lx %c %127s", &addr, &type, name) < 3)
        {
            continue;
        }
        if (addr == 0)
        {
            // kptr_restrict hides real addresses
            continue;
        }
        if (type != 'T' && type != 't')
        {
            // data symbols interleaved with text corrupt nearest-preceding-symbol lookup
            continue;
        }

        kallsyms.emplace_back(addr, name);
    }

    ::fclose(f);
    return 0;
}

const std::string & Symbolizer::resolve(uint64_t addr)
{
    auto cached = cache.find(addr);
    if (cached != cache.end())
    {
        return cached->second;
    }

    std::string result;

    // 0xffff000000000000 is the user/kernel boundary on aarch64 (48-bit VA) and lies
    // in the non-canonical hole on x86-64 -- no valid user address reaches this range.
    if (!kallsyms.empty() && addr >= 0xffff000000000000ULL)
    {
        auto it = std::upper_bound(
            kallsyms.begin(), kallsyms.end(), addr, [](uint64_t a, const std::pair<uint64_t, std::string> & sym) { return a < sym.first; });
        if (it != kallsyms.begin())
        {
            --it;
            result = it->second;
        }
        if (result.empty())
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%lx", addr);
            result = buf;
        }
        auto [it2, _] = cache.emplace(addr, std::move(result));
        return it2->second;
    }

    bool mappingFound = false;
    uint64_t mappingElfAddr = 0;
    std::string_view mappingPath;
    for (const Mapping & m : mappings)
    {
        if (addr < m.start || addr >= m.end)
        {
            continue;
        }
        mappingFound = true;
        mappingElfAddr = addr - m.start + m.fileOffset;
        mappingPath = m.path;

        // one backtrace_state per DSO path, created lazily on first address lookup
        backtrace_state * state;
        auto it = states.find(m.path);
        if (it == states.end())
        {
            state = backtrace_create_state(m.path.c_str(), 0, backtraceErrorCb, nullptr);
            states[m.path] = state;
        }
        else
        {
            state = it->second;
        }

        if (!state)
        {
            SILK_WARN("0x%lx: no backtrace state for %s", addr, m.path.c_str());
            break;
        }

        uint64_t elfAddr = mappingElfAddr;

        SymResult sym;

        // Try runtime address first: works for same-process binaries where
        // dl_iterate_phdr stores symbols at actual load addresses.
        backtrace_syminfo(state, addr, syminfoCallback, backtraceErrorCb, &sym);

        // ELF VMA: works for foreign binaries (our libbacktrace patch uses zero base).
        if (!sym.found)
        {
            backtrace_syminfo(state, elfAddr, syminfoCallback, backtraceErrorCb, &sym);
        }

        // Shared libraries loaded in both target and profiler: dl_iterate_phdr stores
        // symbols at the PROFILER's runtime address (profiler_base + elf_vma).
        auto selfIt = selfBase.find(m.path);
        if (!sym.found && selfIt != selfBase.end())
        {
            backtrace_syminfo(state, selfIt->second + elfAddr, syminfoCallback, backtraceErrorCb, &sym);
        }

        // DWARF fallback via backtrace_pcinfo.
        if (!sym.found)
        {
            backtrace_pcinfo(state, elfAddr, pcinfoCallback, backtraceErrorCb, &sym);
        }
        if (!sym.found && selfIt != selfBase.end())
        {
            backtrace_pcinfo(state, selfIt->second + elfAddr, pcinfoCallback, backtraceErrorCb, &sym);
        }

        if (sym.found)
        {
            int status = 0;
            char * demangled = abi::__cxa_demangle(sym.name.c_str(), nullptr, nullptr, &status);
            if (demangled && status == 0)
            {
                // strip parameter list so overloads collapse to one node in the flamegraph
                const char * paren = ::strchr(demangled, '(');
                result = paren ? std::string(demangled, paren - demangled) : std::string(demangled);
                std::free(demangled);
            }
            else
            {
                result = std::move(sym.name);
            }
        }

        break;
    }

    if (result.empty())
    {
        if (mappingFound)
        {
            SILK_DEBUG(
                "0x%lx: in mapping but not symbolized (elfAddr=0x%lx %.*s)",
                addr,
                mappingElfAddr,
                static_cast<int>(mappingPath.size()),
                mappingPath.data());
            std::string_view base = mappingPath;
            if (auto slash = base.rfind('/'); slash != std::string_view::npos)
            {
                base = base.substr(slash + 1);
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%.*s+0x%lx", static_cast<int>(base.size()), base.data(), mappingElfAddr);
            result = buf;
        }
        else
        {
            SILK_DEBUG("0x%lx: no mapping (library loaded after readMappings?)", addr);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%lx", addr);
            result = buf;
        }
    }

    auto [it, _] = cache.emplace(addr, std::move(result));
    return it->second;
}

void Symbolizer::backtraceErrorCb(void * data, const char * msg, int errnum)
{
    SILK_UNUSED(data);

    if (errnum)
    {
        SILK_ERROR("libbacktrace: %s (%s)", msg, ::strerror(errnum));
    }
    else
    {
        SILK_ERROR("libbacktrace: %s", msg);
    }
}

int Symbolizer::pcinfoCallback(void * data, uintptr_t pc, const char * filename, int lineno, const char * function)
{
    SILK_UNUSED(pc);
    SILK_UNUSED(filename);
    SILK_UNUSED(lineno);

    SymResult * result = static_cast<SymResult *>(data);
    if (function)
    {
        result->name = function;
        result->found = true;
        return 1;
    }
    return 0;
}

void Symbolizer::syminfoCallback(void * data, uintptr_t pc, const char * symname, uintptr_t symval, uintptr_t symsize)
{
    SILK_UNUSED(pc);
    SILK_UNUSED(symval);
    SILK_UNUSED(symsize);

    SymResult * result = static_cast<SymResult *>(data);
    if (symname)
    {
        result->name = symname;
        result->found = true;
    }
}
