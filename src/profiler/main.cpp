#include "profiler.h"
#include "symbolizer.h"

#include <silk/util/logger.h>

#include <boost/program_options.hpp>

#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>

#include <pthread.h>
#include <signal.h>

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

    namespace po = boost::program_options;
    po::options_description desc("profiler options");

    // clang-format off
    desc.add_options()
        ("help,h",                                              "show this help")
        ("pid",           po::value(&targetPid)->required(),         "target process ID")
        ("hz",            po::value(&sampleHz)->default_value(sampleHz),   "on-CPU sampling frequency")
        ("duration",      po::value(&durationSec)->default_value(durationSec), "run for N seconds (0 = until Ctrl+C)")
        ("kernel-stacks", po::bool_switch(&kernelStacks),            "capture kernel stack frames")
        ("on-cpu",        po::bool_switch(&oncpu),                   "capture on-CPU stack samples")
        ("off-cpu",       po::bool_switch(&offcpu),                  "capture off-CPU blocking time")
        ("verbose,v",     po::bool_switch(&verbose),                 "enable debug logging")
        ;
    // clang-format on

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help"))
        {
            std::cout << "usage: profiler [options]\n" << desc << "\n";
            return 0;
        }
        po::notify(vm);
        if (verbose)
        {
            silk::Logger::setLevel(silk::LogLevel::DEBUG);
        }
    }
    catch (const po::error & ex)
    {
        std::cerr << "error: " << ex.what() << "\n" << desc << "\n";
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
        SILK_ERROR("missing required capabilities (CAP_BPF={}, CAP_PERFMON={})", hasBpf, hasPerfmon);
        SILK_ERROR("run once: sudo setcap cap_bpf,cap_perfmon,cap_syslog+eip {}", argv[0]);
        return 1;
    }

    sigset_t mask = blockSignals();

    Symbolizer symbolizer;

    // Read self mappings needed to resolve shared-library symbols cross-process
    int r = symbolizer.readSelfMappings();
    if (r)
    {
        SILK_ERROR("readSelfMappings: {}", strerror(r));
        return r;
    }

    // Read mappings while the target is alive; /proc/PID/maps disappears on exit.
    r = symbolizer.readMappings(targetPid);
    if (r)
    {
        SILK_ERROR("readMappings: {}", strerror(r));
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
            SILK_WARN("run once: sudo setcap cap_bpf,cap_perfmon,cap_syslog+eip {}", argv[0]);
        }
    }

    Profiler profiler(targetPid, sampleHz, kernelStacks, oncpu, offcpu);

    r = profiler.start();
    if (r)
    {
        SILK_ERROR("profiler start failed: {}", strerror(r));
        return r;
    }

    if (durationSec > 0)
    {
        SILK_INFO("profiling pid {} at {} Hz for {} seconds", targetPid, sampleHz, durationSec);
    }
    else
    {
        SILK_INFO("profiling pid {} at {} Hz -- Ctrl+C to stop", targetPid, sampleHz);
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
        SILK_ERROR("collect: {}", ex.what());
        return 1;
    }

    return 0;
}
