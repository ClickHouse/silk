#include "common.h"

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/init.h>
#include <silk/util/logger.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <cxxopts.hpp>
#include <sys/stat.h>

//
// I/O modes
//

static constexpr uint32_t MODE_RANDREAD = 0;
static constexpr uint32_t MODE_SEQREAD = 1;
static constexpr uint32_t MODE_RANDWRITE = 2;

static const char * modeName(uint32_t mode)
{
    if (mode == MODE_RANDREAD)
    {
        return "randread";
    }
    if (mode == MODE_SEQREAD)
    {
        return "seqread";
    }
    if (mode == MODE_RANDWRITE)
    {
        return "randwrite";
    }
    return "unknown";
}

static bool parseMode(const std::string & name, uint32_t * mode)
{
    if (name == "randread")
    {
        *mode = MODE_RANDREAD;
        return true;
    }
    if (name == "seqread")
    {
        *mode = MODE_SEQREAD;
        return true;
    }
    if (name == "randwrite")
    {
        *mode = MODE_RANDWRITE;
        return true;
    }
    return false;
}

//
// Benchmark
//

class OffsetStrategy
{
public:
    OffsetStrategy() = default;

    OffsetStrategy(uint32_t mode, uint64_t fileSize, uint32_t blockSize, uint32_t jobIndex, uint32_t numJobs)
        : mode(mode)
        , blockSize(blockSize)
        , numBlocks(fileSize / blockSize)
        , seed(static_cast<uint64_t>(jobIndex) * 6364136223846793005ULL + 1442695040888963407ULL)
    {
        uint64_t stripeBlocks = numBlocks / numJobs;
        stripeStart = stripeBlocks * jobIndex;
        stripeEnd = stripeStart + stripeBlocks;
        current = stripeStart;
    }

    uint64_t next()
    {
        if (mode == MODE_SEQREAD)
        {
            if (current >= stripeEnd)
            {
                current = stripeStart;
            }
            return current++ * blockSize;
        }
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return (seed % numBlocks) * blockSize;
    }

private:
    uint32_t mode = 0;
    uint32_t blockSize = 0;
    uint64_t numBlocks = 0;
    uint64_t seed = 0;
    uint64_t stripeStart = 0;
    uint64_t stripeEnd = 0;
    uint64_t current = 0;
};

struct ClientConfig
{
    std::string filename;
    uint32_t numJobs = 16;
    uint32_t iodepth = 1;
    uint32_t blockSize = 4096;
    uint32_t mode = MODE_RANDREAD;
    uint64_t fileSize = 1ULL << 30;
    uint64_t durationNs = 10'000'000'000ULL;
    uint64_t warmupNs = 2'000'000'000ULL;
    bool direct = false;
    bool printCounters = false;
};

class Benchmark
{
public:
    Benchmark(const ClientConfig & cfg, int fd);

    void start();
    void stop();

    std::vector<uint64_t> collectLatencies();

private:
    struct Slot
    {
        silk::FiberScheduler::IoFuture future;
        iovec iov;
        uint64_t startCycles = 0;
    };

    struct Job
    {
        silk::FiberFuture future;
        std::vector<uint64_t> latencies;
        OffsetStrategy strategy;
        std::unique_ptr<char, decltype(&std::free)> bufs{nullptr, std::free};
        std::unique_ptr<Slot[]> slots;
        uint32_t head = 0;
    };

    //
    // Helpers.
    //

    void submit(Job * job, Slot * slot);

    //
    // Fiber main functions.
    //

    struct WorkerFiberParams
    {
        Benchmark * benchmark;
        Job * job;
    };
    static int workerFiberMain(WorkerFiberParams * params) noexcept;

    //
    // State.
    //

    ClientConfig cfg;
    int fd;
    std::vector<Job> jobs;
    std::atomic<uint64_t> warmupEndCycles{UINT64_MAX};
    std::atomic<bool> stopping{};
};

Benchmark::Benchmark(const ClientConfig & cfg, int fd)
    : cfg(cfg)
    , fd(fd)
    , jobs(cfg.numJobs)
{
}

void Benchmark::start()
{
    uint32_t i = 0;
    for (Job & job : jobs)
    {
        job.strategy = OffsetStrategy(cfg.mode, cfg.fileSize, cfg.blockSize, i++, static_cast<uint32_t>(jobs.size()));

        // O_DIRECT requires 512-byte-aligned buffers.
        void * rawBufs = std::aligned_alloc(512, static_cast<uint64_t>(cfg.iodepth) * cfg.blockSize);
        SILK_ASSERT(rawBufs);
        std::memset(rawBufs, 0xAB, static_cast<uint64_t>(cfg.iodepth) * cfg.blockSize);

        job.bufs = {static_cast<char *>(rawBufs), std::free};

        job.slots = std::make_unique<Slot[]>(cfg.iodepth);
        for (uint32_t s = 0; s < cfg.iodepth; ++s)
        {
            job.slots[s].iov.iov_base = job.bufs.get() + static_cast<uint64_t>(s) * cfg.blockSize;
            job.slots[s].iov.iov_len = cfg.blockSize;
        }
    }

    for (Job & job : jobs)
    {
        int r = silk::FiberScheduler::run(workerFiberMain, {this, &job}, &job.future);
        SILK_ASSERT(!r, "cannot start fiber: %s", std::strerror(r));
    }

    warmupEndCycles.store(silk::Tsc::getCycles() + silk::Tsc::nanosecondsToCycles(cfg.warmupNs), std::memory_order_relaxed);
}

void Benchmark::stop()
{
    stopping.store(true, std::memory_order_relaxed);

    for (Job & job : jobs)
    {
        job.future.wait();
    }
}

std::vector<uint64_t> Benchmark::collectLatencies()
{
    std::vector<uint64_t> all;
    for (Job & job : jobs)
    {
        all.insert(all.end(), job.latencies.begin(), job.latencies.end());
    }
    return all;
}

void Benchmark::submit(Job * job, Slot * slot)
{
    slot->startCycles = silk::Tsc::getCycles();
    uint64_t offset = job->strategy.next();
    if (cfg.mode == MODE_RANDWRITE)
    {
        silk::FiberScheduler::write(fd, &slot->iov, 1, offset, nullptr, &slot->future);
    }
    else
    {
        silk::FiberScheduler::read(fd, &slot->iov, 1, offset, nullptr, &slot->future);
    }
}

int Benchmark::workerFiberMain(WorkerFiberParams * params) noexcept
{
    Benchmark * benchmark = params->benchmark;
    Job * job = params->job;

    // Pre-fill the ring with iodepth outstanding IOs.
    for (uint32_t i = 0; i < benchmark->cfg.iodepth; ++i)
    {
        benchmark->submit(job, &job->slots[i]);
    }

    while (!benchmark->stopping.load(std::memory_order_relaxed))
    {
        Slot * slot = &job->slots[job->head];

        // Wait for the oldest outstanding IO.
        int r = slot->future.wait();
        if (r)
        {
            SILK_ERROR("request failed: %s", std::strerror(r));
            break;
        }

        uint64_t end = silk::Tsc::getCycles();
        if (slot->startCycles >= benchmark->warmupEndCycles.load(std::memory_order_relaxed))
        {
            job->latencies.push_back(silk::Tsc::cyclesToNanoseconds(end - slot->startCycles));
        }

        // Reuse this slot: reset the future and submit the next IO.
        slot->future.reset();
        benchmark->submit(job, slot);
        job->head = (job->head + 1) % benchmark->cfg.iodepth;
    }

    // Drain remaining in-flight IOs.
    for (uint32_t i = 0; i < benchmark->cfg.iodepth; ++i)
    {
        int r = job->slots[i].future.wait();
        if (r)
        {
            SILK_ERROR("request failed: %s", std::strerror(r));
        }
    }

    return 0;
}

static void printJson(std::vector<uint64_t> & latNs, const ClientConfig & cfg)
{
    uint64_t total = latNs.size();
    double durationS = static_cast<double>(cfg.durationNs) / 1e9;
    double rps = static_cast<double>(total) / durationS;
    double bwBytesS = rps * cfg.blockSize;

    printf("{\n");
    printf("  \"numjobs\": %u,\n", cfg.numJobs);
    printf("  \"iodepth\": %u,\n", cfg.iodepth);
    printf("  \"block_size_bytes\": %u,\n", cfg.blockSize);
    printf("  \"mode\": \"%s\",\n", modeName(cfg.mode));
    printf("  \"file_size_bytes\": %lu,\n", cfg.fileSize);
    printf("  \"duration_s\": %.3f,\n", durationS);
    printf("  \"total\": %lu,\n", total);
    printf("  \"rps\": %.1f,\n", rps);
    printf("  \"bw_bytes\": %.0f,\n", bwBytesS);
    printLatencyUs(latNs);
    if (cfg.printCounters)
    {
        printf(",");
        printSchedulerLatency();
        printf(",");
        printCounters();
    }
    printf("}\n");
}

/**
 * Main entry point.
 */
int main(int argc, char ** argv)
{
    ClientConfig cfg;
    std::string bsStr = "4k";
    std::string rwStr = "randread";
    std::string sizeStr = "1g";
    std::string runtimeStr = "10s";
    std::string warmupStr = "2s";
    bool verbose = false;

    cxxopts::Options cli("file-perf", "file-perf options");

    // clang-format off
    cli.add_options()
        ("h,help",         "show this help")
        ("numjobs",        "number of concurrent worker fibers",                                            cxxopts::value<uint32_t>(cfg.numJobs))
        ("iodepth",        "per-fiber IO queue depth",                                                      cxxopts::value<uint32_t>(cfg.iodepth))
        ("bs",             "block size (e.g. 4k, 1m)",                                                      cxxopts::value<std::string>(bsStr))
        ("rw",             "I/O mode: randread | seqread | randwrite",                                      cxxopts::value<std::string>(rwStr))
        ("size",           "file size (e.g. 1g, 512m)",                                                     cxxopts::value<std::string>(sizeStr))
        ("runtime",        "measurement duration (e.g. 10s, 500ms)",                                        cxxopts::value<std::string>(runtimeStr))
        ("warmup",         "warmup duration (e.g. 2s, 500ms)",                                              cxxopts::value<std::string>(warmupStr))
        ("filename",       "file path",                                                                     cxxopts::value<std::string>(cfg.filename))
        ("direct",         "use O_DIRECT (bypass page cache)",                                              cxxopts::value<bool>(cfg.direct))
        ("print-counters", "enable per-CPU profiler and include counters in the JSON report",               cxxopts::value<bool>(cfg.printCounters))
        ("v,verbose",      "enable debug logging",                                                          cxxopts::value<bool>(verbose))
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
        if (result.count("filename") == 0)
        {
            std::cerr << "error: --filename is required\n" << cli.help() << "\n";
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

    if (!parseMode(rwStr, &cfg.mode))
    {
        std::cerr << "error: unknown --rw mode: " << rwStr << " (expected randread | seqread | randwrite)\n";
        return 1;
    }

    cfg.blockSize = static_cast<uint32_t>(parseSize(bsStr));
    cfg.fileSize = parseSize(sizeStr);
    cfg.durationNs = parseDuration(runtimeStr);
    cfg.warmupNs = parseDuration(warmupStr);
    SILK_ASSERT(cfg.fileSize >= cfg.blockSize);
    SILK_ASSERT(cfg.fileSize / cfg.blockSize >= cfg.numJobs);

    int openFlags = O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | (cfg.direct ? O_DIRECT : 0);
    int fd = ::open(cfg.filename.c_str(), openFlags, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    SILK_ASSERT(fd >= 0, "open failed: %s", std::strerror(errno));

    struct stat st;
    int r = ::fstat(fd, &st);
    SILK_ASSERT(!r, "fstat failed: %s", std::strerror(errno));

    r = ::fallocate(fd, 0, 0, static_cast<off_t>(cfg.fileSize));
    SILK_ASSERT(!r, "fallocate failed: %s", std::strerror(errno));

    sigset_t mask = blockSignals();

    silk::initialize();
    // Disable mid-drain submit batching: file workloads (especially tmpfs)
    // hit io_uring's inline-completion fast path on submit; deferring the
    // syscall through a dispatch-batch threshold pushes those completions off
    // the fast path and costs 25-37% on parallel randread (see docs/perf.md).
    silk::FiberScheduler::Options options{
        .ioUringFlushThreshold = 1,
        .enableProfiler = cfg.printCounters,
    };
    silk::FiberScheduler::initialize(&options);

    SILK_INFO(
        "starting benchmark on: %s  size=%.1fGiB%s",
        cfg.filename.c_str(),
        static_cast<double>(cfg.fileSize) / (1024.0 * 1024 * 1024),
        cfg.direct ? "  direct" : "");

    Benchmark benchmark(cfg, fd);

    SILK_INFO("starting benchmark");
    benchmark.start();

    bool signalled = false;

    if (cfg.warmupNs > 0)
    {
        SILK_INFO("warming up for %s...", formatDuration(cfg.warmupNs).c_str());
        signalled = sigwaitFor(mask, cfg.warmupNs);
    }

    if (!signalled)
    {
        SILK_INFO("measuring for %s...", formatDuration(cfg.durationNs).c_str());
        sigwaitFor(mask, cfg.durationNs);
    }

    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);

    SILK_INFO("stopping benchmark");
    benchmark.stop();

    std::vector<uint64_t> allLat = benchmark.collectLatencies();
    printJson(allLat, cfg);

    silk::FiberScheduler::destroy();
    silk::destroy();

    ::close(fd);
    return 0;
}
