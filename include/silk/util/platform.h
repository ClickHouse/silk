#pragma once

#include <silk/util/assert.h>

#include <bit>
#include <cstddef>
#include <cstdint>

#include <sched.h>
#include <time.h>
#include <unistd.h>

/**
 * On older sysroots we fall back to a minimal in-tree `struct rseq` matching
 * the kernel UAPI layout (cpu_id_start at offset 0, cpu_id at offset 4) and
 * alias `__rseq_offset` to librseq's `rseq_offset`, which librseq populates.
 */
#if __has_include(<sys/rseq.h>)
#    include <sys/rseq.h>
#else
#    include <stddef.h>
struct rseq
{
    uint32_t cpu_id_start;
    uint32_t cpu_id;
};
extern "C" ptrdiff_t rseq_offset;
#    define __rseq_offset rseq_offset
#endif

/** Suppress unused-variable warnings. */
#define SILK_UNUSED(x) (void)(x)

namespace silk
{

/** System page size in bytes. */
static constexpr uint64_t PAGE_SIZE = 4096;

/** Cache line size in bytes. */
static constexpr uint64_t CACHELINE_SIZE = 64;

/** Round @p value up to the nearest multiple of @p align (must be a power of two). */
template <typename T>
static constexpr T alignUp(T value, T align) noexcept
{
    SILK_ASSERT_DEBUG(align != 0 && (align & (align - 1)) == 0);
    return (value + align - 1) & ~(align - 1);
}

/** Round @p value down to the nearest multiple of @p align (must be a power of two). */
template <typename T>
static constexpr T alignDown(T value, T align) noexcept
{
    SILK_ASSERT_DEBUG(align != 0 && (align & (align - 1)) == 0);
    return value & ~(align - 1);
}

/**
 * Return the byte offset of a member within its containing object.
 * C++ equivalent of offsetof, but works with a pointer-to-member instead of a name.
 *
 * Relies on the GCC/Clang ABI where a pointer-to-data-member of a simple class is
 * represented as a byte offset, extracted here via bit_cast.
 */
template <typename T, typename M>
static constexpr ptrdiff_t memberOffset(M T::* memberPtr) noexcept
{
    return std::bit_cast<ptrdiff_t>(memberPtr);
}

/** Return a pointer to the object of type T that contains @p member at offset @p memberPtr. */
template <typename T, typename M>
static T * containerOf(M * member, M T::* memberPtr) noexcept
{
    return reinterpret_cast<T *>(reinterpret_cast<uint8_t *>(member) - memberOffset(memberPtr));
}

/** Return the total number of online processors. */
static inline uint32_t getProcessorCount() noexcept
{
    return static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN));
}

/** Return the number of processors available to the calling process (respects taskset/cgroup affinity). */
static inline uint32_t getAvailableProcessorCount() noexcept
{
    cpu_set_t cpuSet;
    sched_getaffinity(0, sizeof(cpuSet), &cpuSet);
    return static_cast<uint32_t>(CPU_COUNT(&cpuSet));
}

/** Return the index of the CPU the calling thread is currently running on. */
static inline uint32_t getCurrentProcessor() noexcept
{
    // glibc's sched_getcpu fast path is THREAD_GETMEM_VOLATILE(THREAD_SELF, rseq_area.cpu_id),
    // which is the same rseq read we do here. We skip the function call and the cpu_id >= 0
    // fallback to vDSO/syscall -- safe on Linux 4.18+ / glibc 2.35+ where rseq is always registered.
    struct rseq * rseq = reinterpret_cast<struct rseq *>(reinterpret_cast<char *>(__builtin_thread_pointer()) + __rseq_offset);
    return rseq->cpu_id;
}

/** Yield the calling thread's remaining timeslice to the OS scheduler. */
static inline void schedYield() noexcept
{
    sched_yield();
}

/**
 * Emit a CPU pause hint to reduce power and improve pipeline behaviour in spin loops.
 *
 * On aarch64 we use ISB rather than YIELD. YIELD is a NOP on non-SMT cores
 * (which includes all Graviton generations), so it provides no backoff at all.
 * ISB flushes the pipeline and introduces ~13 ns of delay, making it the
 * functional equivalent of x86 PAUSE on aarch64.
 * See: https://bugs.openjdk.org/browse/JDK-8277137
 */
static inline void cpuPause() noexcept
{
#if defined(__x86_64__)
    __asm__ volatile("pause");
#elif defined(__aarch64__)
    __asm__ volatile("isb");
#else
#    error Unsupported platform
#endif
}

/** Return the current time as nanoseconds since an arbitrary epoch (CLOCK_MONOTONIC). */
static inline uint64_t getTimeNanoseconds() noexcept
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000 + static_cast<uint64_t>(ts.tv_nsec);
}

/** 64-bit integer hash (Murmur3 fmix64 finalizer). */
static constexpr uint64_t intHash(uint64_t key) noexcept
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return key;
}

} // namespace silk
