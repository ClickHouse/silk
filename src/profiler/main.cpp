#include "profiler.h"
#include "symbolizer.h"

#include <silk/util/logger.h>

#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>

#include <pthread.h>
#include <signal.h>

#include <cxxopts.hpp>
#include <linux/capability.h>
#include <sys/syscall.h>

static bool hasCapability(int cap) noexcept
{
    __user_cap_header_struct header = {};
    header.version = _LINUX_CAPABILITY_VERSION_3;

    __user_cap_data_struct data[2] = {};
    if (::syscall(SYS_capget, &header, data) != 0)
    {
        return false;
    }

    uint32_t idx = (uint32_t)cap / 32;
    uint32_t bit = (uint32_t)cap % 32;
    return (data[idx].effective >> bit) & 1;
}

static sigset_t blockSignals() noexcept
{
    sigset_t mask;
    ::sigemptyset(&mask);
    ::sigaddset(&mask, SIGINT);
    ::sigaddset(&mask, SIGTERM);
    ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    return mask;
}

// Waits up to ns nanoseconds for a signal in mask.
// Returns true if a signal was received, false if the timeout expired.
// Pass UINT64_MAX to wait indefinitely.
static bool sigwaitFor(const sigset_t & mask, uint64_t ns) noexcept
{
    if (ns == UINT64_MAX)
    {
        int sig;
        ::sigwait(&mask, &sig);
        return true;
    }

    struct timespec timeout = {
        .tv_sec = static_cast<time_t>(ns / 1'000'000'000ULL),
        .tv_nsec = static_cast<long>(ns % 1'000'000'000ULL),
    };

    return ::sigtimedwait(&mask, nullptr, &timeout) > 0;
}

int main(int argc, char ** argv)
{
    uint32_t targetPid = 0;
    uint32_t sampleHz = 99;
    uint32_t durationSec = 0;
    bool kernelStacks = false;
    bool oncpu = false;
    bool offcpu = false;
    bool verbose = false;

    cxxopts::Options cli("profiler", "profiler options");

    // clang-format off
    cli.add_options()
        ("h,help",        "show this help")
        ("pid",           "target process ID",                          cxxopts::value<uint32_t>(targetPid))
        ("hz",            "on-CPU sampling frequency",                  cxxopts::value<uint32_t>(sampleHz))
        ("duration",      "run for N seconds (0 = until Ctrl+C)",       cxxopts::value<uint32_t>(durationSec))
        ("kernel-stacks", "capture kernel stack frames",                cxxopts::value<bool>(kernelStacks))
        ("on-cpu",        "capture on-CPU stack samples",               cxxopts::value<bool>(oncpu))
        ("off-cpu",       "capture off-CPU blocking time",              cxxopts::value<bool>(offcpu))
        ("v,verbose",     "enable debug logging",                       cxxopts::value<bool>(verbose))
        ;
    // clang-format on

    try
    {
        auto result = cli.parse(argc, argv);
        if (result.count("help"))
        {
            std::cout << cli.help() << "\n";
            return 0;
        }
        if (result.count("pid") == 0)
        {
            std::cerr << "error: --pid is required\n" << cli.help() << "\n";
            return 1;
        }
        if (verbose)
        {
            silk::Logger::setLevel(silk::LogLevel::DEBUG);
        }
    }
    catch (const cxxopts::exceptions::exception & ex)
    {
        std::cerr << "error: " << ex.what() << "\n" << cli.help() << "\n";
        return 1;
    }

    if (!oncpu && !offcpu)
    {
        SILK_ERROR("nothing to capture; pass at least one of --on-cpu, --off-cpu");
        return 1;
    }

    bool hasBpf = hasCapability(CAP_BPF);
    bool hasPerfmon = hasCapability(CAP_PERFMON);

    if (!hasBpf || !hasPerfmon)
    {
        SILK_ERROR("missing required capabilities (CAP_BPF=%d, CAP_PERFMON=%d)", hasBpf, hasPerfmon);
        SILK_ERROR("run once: sudo setcap cap_bpf,cap_perfmon,cap_syslog+eip %s", argv[0]);
        return 1;
    }

    sigset_t mask = blockSignals();

    Symbolizer symbolizer;

    // Read self mappings needed to resolve shared-library symbols cross-process
    int r = symbolizer.readSelfMappings();
    if (r)
    {
        SILK_ERROR("readSelfMappings: %s", std::strerror(r));
        return r;
    }

    // Read mappings while the target is alive; /proc/PID/maps disappears on exit.
    r = symbolizer.readMappings(targetPid);
    if (r)
    {
        SILK_ERROR("readMappings: %s", std::strerror(r));
        return r;
    }

    if (kernelStacks)
    {
        // Read kernel symbols if has capability
        if (hasCapability(CAP_SYSLOG))
        {
            symbolizer.readKallsyms();
        }
        else
        {
            SILK_WARN("CAP_SYSLOG missing -- kernel frames will appear as hex addresses");
            SILK_WARN("run once: sudo setcap cap_bpf,cap_perfmon,cap_syslog+eip %s", argv[0]);
        }
    }

    Profiler profiler(targetPid, sampleHz, kernelStacks, oncpu, offcpu);

    r = profiler.start();
    if (r)
    {
        SILK_ERROR("profiler start failed: %s", std::strerror(r));
        return r;
    }

    if (durationSec > 0)
    {
        SILK_INFO("profiling pid %u at %u Hz for %u seconds", targetPid, sampleHz, durationSec);
    }
    else
    {
        SILK_INFO("profiling pid %u at %u Hz -- Ctrl+C to stop", targetPid, sampleHz);
    }

    uint64_t waitNs = durationSec > 0 ? (uint64_t)durationSec * 1'000'000'000ULL : UINT64_MAX;
    sigwaitFor(mask, waitNs);

    profiler.stop();

    SILK_INFO("collecting results...");
    try
    {
        profiler.emitFoldedStacks(&symbolizer);
    }
    catch (const std::exception & ex)
    {
        SILK_ERROR("collect: %s", ex.what());
        return 1;
    }

    return 0;
}
