#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

struct bpf_link;
struct profiler_bpf;
class Symbolizer;

class Profiler
{
public:
    Profiler(uint32_t targetTgid, uint32_t sampleHz, bool kernelStacks, bool oncpu, bool offcpu) noexcept;
    ~Profiler() noexcept;

    // opens, loads, and attaches all BPF programs; returns 0 on success, errno on failure
    int start() noexcept;

    // detaches BPF programs; maps remain readable
    void stop() noexcept;

    // Emits merged folded stacks to stdout: "frame1;frame2 on_ns off_ns"
    void emitFoldedStacks(Symbolizer * symbolizer);

private:
    static constexpr int MAX_FRAMES = 127;
    using StackMap = std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>>;
    static void drainStacks(int fd, StackMap & merged, bool isOnCpu, uint64_t weightFactor);
    static int countFrames(const uint64_t * addrs) noexcept;

    uint32_t targetTgid;
    uint32_t sampleHz;
    bool kernelStacks;
    bool oncpu;
    bool offcpu;
    profiler_bpf * skel = nullptr;
    std::vector<bpf_link *> perfLinks;
};
