#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

struct bpf_link;
struct profiler_bpf;
class Symbolizer;

class Profiler
{
public:
    Profiler(uint32_t targetTgid, uint32_t sampleHz, bool kernelStacks, bool oncpu, bool offcpu, bool usdt) noexcept;
    ~Profiler() noexcept;

    // opens, loads, and attaches all BPF programs; returns 0 on success, errno on failure
    int start() noexcept;

    // detaches BPF programs; maps remain readable
    void stop() noexcept;

    // Emits merged folded stacks to stdout: "frame1;frame2 on_ns off_ns"
    void emitFoldedStacks(Symbolizer * symbolizer);

    // Per-category fiber-lifecycle latency breakdown. Reads the latency_hist
    // BPF map populated by the silk:fiber_* USDT handlers. Only meaningful
    // when the profiler was started with usdt=true; otherwise the map is
    // empty and the table prints no rows.
    void emitLatencyBreakdown();

private:
    static constexpr int MAX_FRAMES = 127;
    using StackMap = std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>>;
    static void drainStacks(int fd, StackMap & merged, bool isOnCpu, uint64_t weightFactor);
    static int countFrames(const uint64_t * addrs) noexcept;

    static constexpr int HIST_BUCKETS = 32;
    using HistBuckets = std::array<uint64_t, HIST_BUCKETS>;
    using LatencyHist = std::map<std::pair<uint8_t, uint8_t>, HistBuckets>;
    static void drainHistogram(int fd, LatencyHist & hist);
    static uint64_t percentileNs(const HistBuckets & buckets, double p) noexcept;
    static uint64_t maxNs(const HistBuckets & buckets) noexcept;
    static const char * phaseName(uint8_t phase) noexcept;

    uint32_t targetTgid;
    uint32_t sampleHz;
    bool kernelStacks;
    bool oncpu;
    bool offcpu;
    bool usdt;
    profiler_bpf * skel = nullptr;
    std::vector<bpf_link *> perfLinks;
    std::vector<bpf_link *> usdtLinks;
};
