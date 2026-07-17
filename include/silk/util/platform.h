#pragma once

#include <silk/util/assert.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

/** Token-paste two preprocessor arguments after expansion. */
#define SILK_CONCAT_IMPL(a, b) a##b
#define SILK_CONCAT(a, b) SILK_CONCAT_IMPL(a, b)

namespace silk
{

/** System page size in bytes. */
#if defined(PAGE_SIZE)
static constexpr uint64_t kPageSize = PAGE_SIZE;
#else
static constexpr uint64_t kPageSize = 4096;
#endif

/** Cache line size in bytes. */
#if defined(CACHE_LINESIZE)
static constexpr uint64_t kCacheLineSize = CACHE_LINESIZE;
#else
static constexpr uint64_t kCacheLineSize = 64;
#endif

/** Hard cap on CPU index (largest known socket: 384 cores). */
static constexpr uint16_t kInvalidProcessorNumber = (1 << 10);

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

/** Round @p value up to the nearest multiple of @p align (must be a power of two). */
template <typename T>
static T * alignUp(T * ptr, size_t align) noexcept
{
    return reinterpret_cast<T *>(alignUp<uintptr_t>(reinterpret_cast<uintptr_t>(ptr), align));
}

/** Round @p value down to the nearest multiple of @p align (must be a power of two). */
template <typename T>
static T * alignDown(T * ptr, size_t align) noexcept
{
    return reinterpret_cast<T *>(alignDown<uintptr_t>(reinterpret_cast<uintptr_t>(ptr), align));
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
static inline uint16_t getProcessorCount() noexcept
{
    return static_cast<uint16_t>(sysconf(_SC_NPROCESSORS_ONLN));
}

/** Return the number of processors available to the calling process (respects taskset/cgroup affinity). */
static inline uint16_t getAvailableProcessorCount() noexcept
{
    cpu_set_t cpuSet;
    sched_getaffinity(0, sizeof(cpuSet), &cpuSet);
    return static_cast<uint16_t>(CPU_COUNT(&cpuSet));
}

/** Return the index of the CPU the calling thread is currently running on. */
static inline uint16_t getCurrentProcessor() noexcept
{
    // The thread pointer is read through volatile asm rather than __builtin_thread_pointer(). A fiber
    // may suspend on one OS thread and resume on another (work-stealing, or the SQ-ring-overflow yield
    // in enqueueIo), which changes the thread pointer mid-function. The C++ abstract machine assumes the
    // thread pointer is invariant for a thread's lifetime, so with __builtin_thread_pointer() the compiler
    // is free to hoist the read out of a migration-spanning loop and keep the pre-migration value in a
    // callee-saved register (which jump_fcontext preserves across the switch), reading the wrong CPU's
    // rseq area after the migration. The volatile asm forces a fresh read at every use; it cannot be
    // hoisted or CSE'd across the suspension.
    char * threadPointer;
#if defined(__x86_64__)
    __asm__ volatile("movq %%fs:0, %0" : "=r"(threadPointer));
#elif defined(__aarch64__)
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(threadPointer));
#else
#    error Unsupported platform
#endif

    // glibc's sched_getcpu fast path is THREAD_GETMEM_VOLATILE(THREAD_SELF, rseq_area.cpu_id),
    // which is the same rseq read we do here. We skip the function call and the cpu_id >= 0
    // fallback to vDSO/syscall -- safe on Linux 4.18+ / glibc 2.35+ where rseq is always registered.
    struct rseq * rseq = reinterpret_cast<struct rseq *>(threadPointer + __rseq_offset);
    return static_cast<uint16_t>(rseq->cpu_id);
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

/** Thread-safe strerror_r into a caller-provided buffer; may return nullptr for an unknown code. */
static inline const char * strerror(int code, char * buf, size_t size) noexcept
{
#if defined(_GNU_SOURCE)
    return ::strerror_r(code, buf, size);
#else
    return ::strerror_r(code, buf, size) == 0 ? buf : nullptr;
#endif
}

} // namespace silk
