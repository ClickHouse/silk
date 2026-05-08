#include "profiler.h"
#include "symbolizer.h"

#include <silk/util/assert.h>
#include <silk/util/logger.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/perf_event.h>
#include <linux/types.h>
#include <sys/syscall.h>

// BPF skeleton uses kernel integer aliases; define them for user-space.
typedef __u8 u8;
typedef __u16 u16;
typedef __u32 u32;
typedef __u64 u64;

#include <profiler.skel.h>

static int perfEventOpen(perf_event_attr * attr, pid_t pid, int cpu, int groupFd, unsigned long flags)
{
    return (int)::syscall(__NR_perf_event_open, attr, pid, cpu, groupFd, flags);
}

static int libbpfPrint(libbpf_print_level level, const char * format, va_list args)
{
    silk::LogLevel logLevel;
    if (level == LIBBPF_DEBUG)
    {
        logLevel = silk::LogLevel::DEBUG;
    }
    else if (level == LIBBPF_INFO)
    {
        logLevel = silk::LogLevel::INFO;
    }
    else if (level == LIBBPF_WARN)
    {
        logLevel = silk::LogLevel::WARN;
    }
    else
    {
        logLevel = silk::LogLevel::ERROR;
    }

    if (silk::Logger::isEnabled(logLevel))
    {
        char buf[256];
        int len = std::vsnprintf(buf, sizeof(buf), format, args);
        if (len > 0)
        {
            if (len >= (int)sizeof(buf))
            {
                len = (int)sizeof(buf) - 1;
            }
            if (buf[len - 1] == '\n')
            {
                buf[len - 1] = '\0';
            }
        }
        silk::Logger::log(logLevel, __FILE__, __LINE__, buf);
    }

    return 0;
}

Profiler::Profiler(uint32_t targetTgid, uint32_t sampleHz, bool kernelStacks, bool oncpu, bool offcpu) noexcept
    : targetTgid(targetTgid)
    , sampleHz(sampleHz)
    , kernelStacks(kernelStacks)
    , oncpu(oncpu)
    , offcpu(offcpu)
{
}

Profiler::~Profiler() noexcept
{
    stop();
    if (skel)
    {
        profiler_bpf__destroy(skel);
        skel = nullptr;
    }
}

int Profiler::start() noexcept
{
    // RLIMIT_MEMLOCK no longer applies to BPF allocations since Linux 5.11.
    libbpf_set_print(libbpfPrint);

    skel = profiler_bpf__open();
    if (!skel)
    {
        SILK_ERROR("failed to open BPF skeleton");
        return EINVAL;
    }

    skel->rodata->target_tgid = targetTgid;
    skel->rodata->kernel_stacks = kernelStacks ? 1 : 0;

    if (!oncpu)
    {
        bpf_program__set_autoload(skel->progs.on_cpu_sample, false);
    }
    if (!offcpu)
    {
        bpf_program__set_autoload(skel->progs.on_sched_switch, false);
    }

    if (profiler_bpf__load(skel))
    {
        SILK_ERROR("failed to load BPF programs");
        stop();
        return EINVAL;
    }

    if (profiler_bpf__attach(skel))
    {
        SILK_ERROR("failed to attach BPF programs");
        stop();
        return EINVAL;
    }

    if (oncpu)
    {
        perf_event_attr attr = {};
        attr.type = PERF_TYPE_SOFTWARE;
        attr.config = PERF_COUNT_SW_CPU_CLOCK;
        attr.freq = 1;
        attr.sample_freq = sampleHz;

        int numCpus = libbpf_num_possible_cpus();
        for (int cpu = 0; cpu < numCpus; cpu++)
        {
            int fd = perfEventOpen(&attr, -1, cpu, -1, 0);
            if (fd < 0)
            {
                int r = errno;
                if (r == ENODEV)
                {
                    // CPU offline
                    continue;
                }
                SILK_WARN("perf_event_open cpu {}: {}", cpu, strerror(r));
                continue;
            }

            bpf_link * link = bpf_program__attach_perf_event(skel->progs.on_cpu_sample, fd);
            if (!link)
            {
                int r = errno;
                SILK_WARN("attach perf_event cpu {}: {}", cpu, strerror(r));
                ::close(fd);
                continue;
            }

            perfLinks.push_back(link);
        }

        if (perfLinks.empty())
        {
            SILK_ERROR("failed to attach to any CPU");
            stop();
            return ENODEV;
        }
    }

    return 0;
}

void Profiler::stop() noexcept
{
    for (bpf_link * link : perfLinks)
    {
        bpf_link__destroy(link);
    }
    perfLinks.clear();
}

void Profiler::emitFoldedStacks(Symbolizer * symbolizer)
{
    SILK_ASSERT(skel);

    int stackFd = bpf_map__fd(skel->maps.stack_map);
    int oncpuFd = bpf_map__fd(skel->maps.oncpu);
    int offcpuFd = bpf_map__fd(skel->maps.offcpu);

    uint64_t sampleNs = 1'000'000'000ULL / sampleHz;

    // Merge both maps into {stack_key → (on_ns, off_ns)}.
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> merged;

    drainStacks(oncpuFd, merged, true, sampleNs);
    drainStacks(offcpuFd, merged, false, 1);

    std::string folded;

    for (auto & [key, times] : merged)
    {
        auto [on_ns, off_ns] = times;

        uint32_t userSid = (uint32_t)(key >> 32);
        uint32_t kernelSid = (uint32_t)(key & 0xFFFFFFFF);

        uint64_t userAddrs[MAX_FRAMES] = {};
        uint64_t kernelAddrs[MAX_FRAMES] = {};

        // UINT32_MAX is the sentinel the BPF program stores when bpf_get_stackid
        // fails (normalizes all negative errno values to -1, which becomes
        // 0xFFFFFFFF when cast to u32). Valid stack IDs are always non-negative
        // s32 values and never alias UINT32_MAX.
        //
        // On lookup failure (stack ID evicted between map iteration and lookup)
        // the arrays remain zero-initialized, countFrames returns 0, and the
        // sample is skipped by the nUser == 0 && nKernel == 0 check below.
        if (userSid != UINT32_MAX)
        {
            (void)bpf_map_lookup_elem(stackFd, &userSid, userAddrs);
        }
        if (kernelSid != UINT32_MAX)
        {
            (void)bpf_map_lookup_elem(stackFd, &kernelSid, kernelAddrs);
        }

        int nUser = countFrames(userAddrs);
        int nKernel = countFrames(kernelAddrs);

        if (nUser == 0 && nKernel == 0)
        {
            continue;
        }

        folded.clear();

        for (int i = nUser - 1; i >= 0; i--)
        {
            // Skip non-canonical addresses (e.g. Boost.Context stack sentinels).
            // x86-64 canonical user-space tops out at 2^47; anything above that
            // and below the kernel range is a synthetic/invalid frame pointer.
            uint64_t addr = userAddrs[i];
            if (addr > 0x0000ffffffffffffULL && addr < 0xffff000000000000ULL)
            {
                continue;
            }

            if (!folded.empty())
            {
                folded += ';';
            }
            folded += symbolizer->resolve(addr);
        }

        if (nKernel > 0)
        {
            folded += ";[k]";
            for (int i = nKernel - 1; i >= 0; i--)
            {
                uint64_t addr = kernelAddrs[i];
                if (addr < 0xffff000000000000ULL)
                {
                    // garbage frame: kernel unwinder hit a function without frame pointers
                    continue;
                }
                const std::string & sym = symbolizer->resolve(addr);
                if (sym == "__init_scratch_end")
                {
                    // kernel frame-pointer sentinel
                    continue;
                }
                folded += ';';
                folded += sym;
            }
        }

        std::printf("%s %lu %lu\n", folded.c_str(), on_ns, off_ns);
    }
}

void Profiler::drainStacks(int fd, StackMap & merged, bool isOnCpu, uint64_t weightFactor)
{
    int numCpus = libbpf_num_possible_cpus();
    std::vector<uint64_t> perCpuValues(numCpus);

    uint64_t key, nextKey;
    if (bpf_map_get_next_key(fd, nullptr, &nextKey) != 0)
    {
        return;
    }

    do
    {
        key = nextKey;
        bpf_map_lookup_elem(fd, &key, perCpuValues.data());

        uint64_t total = 0;
        for (int i = 0; i < numCpus; i++)
        {
            total += perCpuValues[i];
        }

        if (isOnCpu)
        {
            merged[key].first += total * weightFactor;
        }
        else
        {
            merged[key].second += total * weightFactor;
        }

    } while (bpf_map_get_next_key(fd, &key, &nextKey) == 0);
}

int Profiler::countFrames(const uint64_t * addrs) noexcept
{
    int n = 0;
    while (n < MAX_FRAMES && addrs[n])
    {
        n++;
    }
    return n;
}
