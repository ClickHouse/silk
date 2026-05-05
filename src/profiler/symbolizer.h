#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct backtrace_state;

class Symbolizer
{
public:
    Symbolizer() noexcept = default;
    ~Symbolizer() noexcept = default;

    // Parses /proc/PID/maps to build the address-to-DSO mapping.
    // Returns 0 on success, errno on failure.
    int readMappings(uint32_t pid) noexcept;

    // Reads /proc/self/maps to record the profiler's own load base per DSO.
    // Required for resolving shared-library symbols: backtrace_create_state stores
    // them at the profiler's runtime addresses, so we translate target addresses
    // via: profiler_base + (addr - target_base + target_file_offset).
    // Returns 0 on success, errno on failure.
    int readSelfMappings() noexcept;

    // Reads /proc/kallsyms for kernel symbol resolution (requires CAP_SYSLOG).
    // Returns 0 on success, errno on failure.
    int readKallsyms() noexcept;

    // Returns the function name for addr, or "0x<addr>" if unresolvable.
    const std::string & resolve(uint64_t addr);

private:
    struct Mapping
    {
        uint64_t start;
        uint64_t end;
        uint64_t fileOffset;
        std::string path;
    };

    struct SymResult
    {
        std::string name;
        bool found = false;
    };

    static void backtraceErrorCb(void * data, const char * msg, int errnum);
    static void syminfoCallback(void * data, uintptr_t pc, const char * symname, uintptr_t symval, uintptr_t symsize);
    static int pcinfoCallback(void * data, uintptr_t pc, const char * filename, int lineno, const char * function);

    std::vector<Mapping> mappings;
    std::unordered_map<std::string, backtrace_state *> states;
    std::unordered_map<uint64_t, std::string> cache;
    std::vector<std::pair<uint64_t, std::string>> kallsyms; // sorted by address
    // path -> profiler's load base (map_start - file_offset) from /proc/self/maps
    std::unordered_map<std::string, uint64_t> selfBase;
};
