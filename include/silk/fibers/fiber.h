#pragma once

#include <silk/fibers/future.h>
#include <silk/util/stack.h>
#include <silk/util/tree.h>

#include <atomic>
#include <cerrno>
#include <memory>
#include <utility>

#include <sys/uio.h>

namespace silk
{

class Fiber;

/**
 * Maximum size in bytes of the parameter type passed to run().
 * Types exceeding this limit cause a compile-time error.
 */
static constexpr uint64_t FIBER_PARAMETERS_SIZE = 64;

/**
 * Fiber entry point signature. Returns an integer result code.
 */
using FiberMain = int(void * parameters) noexcept;

/**
 * Destructor for the fiber's parameter buffer. Called after the entry point returns.
 * Null for trivially destructible parameter types.
 */
using ParametersDtor = void(void * parameters) noexcept;

/**
 * Packed fiber identity: [category:8 | cpu:10 | counter:46] stored as uint64_t.
 *  category: byte passed to run; the runtime treats it as opaque.
 *  cpu:      CPU on which the fiber's id was created at allocation time.
 *  counter:  per-CPU monotonic counter, fresh on every Fiber init/reuse.
 */
union FiberId
{
    struct
    {
        uint64_t counter : 46;
        uint64_t cpu : 10;
        uint64_t category : 8;
    };
    uint64_t raw;
};
static_assert(sizeof(FiberId) == 8);

/**
 * Kind of a profile event emitted by the scheduler.
 */
enum class ProfileEventKind : uint8_t
{
    READY_WAIT = 0, // enqueueReady -> runFiber: ready-queue dwell
    FIBER_RUN = 1, // switchToFiberContext -> return: on-CPU time per slice
    SUSPEND_WAIT = 2, // suspended -> enqueueReady: blocked-on-condition latency
    IO_WAIT = 3, // enqueueIo -> handleCompletion: full IO latency
    SQ_WAIT = 4, // enqueueIo -> io_uring_submit: SQE pending in silk's SQ ring before flush to kernel
    SUBMIT_IO = 5, // io_uring_submit syscall cost
    CQ_WAIT = 6, // wall-clock gap between consecutive non-empty CQ drains on a ring
    MAX
};

/**
 * Percentile latency report returned by FiberScheduler::reportLatency.
 * All values are in nanoseconds.
 */
struct LatencyReport
{
    uint64_t p50;
    uint64_t p90;
    uint64_t p99;
    uint64_t p999;
    uint64_t count;
};

/**
 * Cooperative fiber scheduler with per-CPU threads, async IO, and work-stealing.
 */
class FiberScheduler
{
public:
    /**
     * Optional knobs passed to initialize.  Add fields with defaults to extend
     * without changing the API.
     */
    struct Options
    {
        // Per-fiber stack size in bytes. Must be a multiple of the system page size.
        // The pool also reserves two guard pages adjacent to each stack.
        uint32_t fiberStackSize = 64 * 1024;

        // Per-CPU ready queue capacity (fibers). Must be a power of two and >= 2.
        // Sized to absorb dispatch bursts without falling back to the global queue.
        uint32_t readyQueueCapacity = 1024;

        // Hash-table size for futex-style waiter lookups. Must be a power of two.
        uint32_t waiterTableSize = 4096;

        // Per-CPU io_uring SQ ring capacity. Must be a power of two; the kernel
        // rounds up to the nearest supported size.
        uint32_t ioUringQueueSize = 256;

        // Upper bound on the number of SQEs that may sit in silk's SQ ring
        // before being flushed to the kernel.  Must be <= ioUringQueueSize.
        uint32_t ioUringFlushThreshold = 64;

        // Upper bound on how long an SQE may sit in silk's SQ ring before
        // being flushed to the kernel.
        uint32_t ioUringFlushTimeout = 100'000;

        // Derived in initialize() from ioUringFlushTimeout; users do not set this.
        uint64_t ioUringFlushTimeoutCycles = 0;

        // Scheduler park backoff (nanoseconds). The dispatch loop spins for up to
        // spinThresholdNs after going idle; past that it parks on the eventfd with
        // an exponential backoff starting at initialWaitNs and capped at maxWaitNs.
        uint32_t initialWaitNs = 1'000;
        uint32_t maxWaitNs = 10'000'000;
        uint32_t spinThresholdNs = 20'000;

        // Allocate per-CPU latency profilers.
        bool enableProfiler = false;

        // Disable work-stealing. Set to study the effect of work-stealing in
        // isolation (e.g. head-of-line blocking benchmarks).
        // Production should leave this off.
        bool disableWorkStealing = false;
    };

    /**
     * Initialize the scheduler and start per-CPU scheduler threads.
     * Must be called once before any other FiberScheduler method.
     * @param options  Optional configuration; defaults are used when null.
     */
    static void initialize(const Options * options = nullptr) noexcept;

    /**
     * Return the active configuration. Set by initialize; immutable thereafter.
     */
    static const Options & getOptions() noexcept { return options; }

    /**
     * Stop all scheduler threads and release all resources.
     * No fibers may be running or scheduled when this is called.
     */
    static void destroy() noexcept;

    /**
     * Start a fiber whose result will be delivered to a FiberFuture.
     *
     * @param fiberMain      Entry point; receives a pointer to a copy of parameters.
     * @param parameters     Moved into the fiber's parameter buffer.
     * @param future         Receives the fiber's integer result on completion.
     * @return               0 on success, or ENOMEM if fiber allocation failed.
     */
    template <typename T>
    [[nodiscard]] static int run(int (*fiberMain)(T *) noexcept, T && parameters, FiberFuture * future) noexcept
    {
        return run(fiberMain, std::forward<T>(parameters), 0, future);
    }

    /**
     * Start a fiber whose result will be delivered to a FiberFuture and stamp
     * the fiber's identity with @p category (FiberId::category field).
     * The runtime treats the category as opaque; profilers and tracers use it
     * to group samples by fiber role.
     */
    template <typename T>
    [[nodiscard]] static int run(int (*fiberMain)(T *) noexcept, T && parameters, uint8_t category, FiberFuture * future) noexcept
    {
        static_assert(sizeof(T) <= FIBER_PARAMETERS_SIZE);
        Fiber * fiber = allocateFiber(
            reinterpret_cast<FiberMain *>(fiberMain),
            std::is_trivially_destructible_v<T> ? nullptr : destroyParameters<T>,
            category,
            future);
        if (fiber)
        {
            std::construct_at(static_cast<T *>(getFiberParameters(fiber)), std::forward<T>(parameters));
            schedule(fiber);
            return 0;
        }
        return ENOMEM;
    }

    /**
     * Start a fiber, block until it completes, and return its result.
     * Suspends the calling fiber cooperatively if called from a fiber context;
     * blocks the calling thread otherwise.
     *
     * @param fiberMain  Entry point; receives a pointer to a copy of parameters.
     * @param parameters Moved into the fiber's parameter buffer.
     * @return           The fiber's integer result code.
     */
    template <typename T>
    static int run(int (*fiberMain)(T *) noexcept, T && parameters, uint8_t category = 0) noexcept
    {
        FiberFuture future;
        int r = run(fiberMain, std::forward<T>(parameters), category, &future);
        return r ? r : future.wait();
    }

    /**
     * Return the current fiber's identity, or a zero-initialized FiberId if
     * the calling thread is not currently inside a fiber's context (e.g. proxy
     * fiber thread, or scheduler thread between fibers).
     */
    static FiberId getCurrentFiberId() noexcept;

    /**
     * Return the Fiber handle for the calling context.
     * Valid from any context, including non-fiber threads.
     */
    static Fiber * getCurrentFiber() noexcept;

    /**
     * Return true if fiber is currently running.
     */
    static bool isFiberRunning(Fiber * fiber) noexcept;

    /**
     * Resume a suspended fiber.
     *
     * @return true  if the fiber was successfully resumed.
     * @return false if the fiber was not in a suspendable state.
     */
    static bool schedule(Fiber * fiber) noexcept;

    /**
     * Suspend the current fiber and immediately reschedule it, giving other
     * fibers a chance to run. No-op when called from a non-fiber thread.
     */
    static void yield() noexcept;

    /**
     * RAII guard that moves the current fiber out of the cooperative scheduler
     * for its lifetime, allowing blocking calls, then returns it on destruction.
     * Must be used from a fiber context (not already in thread mode).
     */
    class [[nodiscard]] ThreadModeScope
    {
    public:
        ThreadModeScope() noexcept { enterThreadMode(); }
        ~ThreadModeScope() noexcept { exitThreadMode(); }

        ThreadModeScope(const ThreadModeScope &) = delete;
        ThreadModeScope & operator=(const ThreadModeScope &) = delete;
    };

    /**
     * Move the current fiber out of the cooperative scheduler so it may make
     * blocking calls or perform heavy CPU work without delaying other fibers.
     * Must be paired with exitThreadMode().
     */
    static void enterThreadMode() noexcept;

    /**
     * Return the current fiber to the cooperative scheduler.
     * Must be called after enterThreadMode().
     */
    static void exitThreadMode() noexcept;

    /**
     * Callback invoked by suspend() after the calling fiber has yielded control.
     * @param fiber    The suspended fiber; call schedule() to resume it.
     * @param context  Caller-supplied context pointer.
     */
    using SuspendCallback = void(Fiber * fiber, void * context) noexcept;

    /**
     * Suspend the current fiber and invoke callback.
     *
     * The callback is responsible for arranging wakeup - typically by registering
     * the fiber as a waiter and calling schedule() when the condition is met.
     * The callback must handle the race where the wakeup arrives before the fiber
     * is fully suspended.
     */
    static void suspend(SuspendCallback * callback, void * context) noexcept;

    /**
     * Park a suspended fiber under key until a matching releaseWaiters() call.
     * Used by synchronization primitives.
     *
     * @param key   Arbitrary key identifying the wait condition (e.g. address of the primitive).
     * @param fiber Suspended fiber to park.
     */
    static void enqueueWaiter(uint64_t key, Fiber * fiber) noexcept;

    /**
     * Resume all fibers parked under key. Spurious wakeups are possible;
     * callers must re-check their wait condition after being woken.
     *
     * @param key  Same key passed to enqueueWaiter().
     */
    static void releaseWaiters(uint64_t key) noexcept;

    /**
     * Completion handle for an async IO operation submitted via read() or write().
     */
    class IoFuture : public FiberFuture
    {
    public:
        /**
         * Request cancellation of the pending IO operation.
         * Returns immediately; the operation's future will complete with ECANCELED
         * (or with the normal result if the operation already completed before the
         * cancellation was processed).
         */
        void cancel() noexcept { FiberScheduler::cancelIo(this); }

    private:
        friend class FiberScheduler;
        uint64_t * result = nullptr;
        uint64_t submitTimestamp = 0;
        uint8_t category = 0;
    };

    /**
     * Blocking read: submit a read into @p buf and suspend the calling fiber
     * until the IO completes.
     *
     * @param fd        File descriptor to read from.
     * @param buf       Destination buffer.
     * @param len       Number of bytes to read.
     * @param offset    Byte offset within the file.
     * @param bytesRead If not null, receives the number of bytes read on success.
     * @return          0 on success, or a errno on failure.
     */
    static int read(int fd, void * buf, uint64_t len, uint64_t offset, uint64_t * bytesRead = nullptr) noexcept
    {
        IoFuture future;
        iovec iov{buf, len};
        read(fd, &iov, 1, offset, bytesRead, &future);
        return future.wait();
    }

    /**
     * Async scatter read. Returns immediately; the caller must wait on @p future
     * for the result.
     *
     * @param fd        File descriptor to read from.
     * @param iov       Array of buffers (scatter list).
     * @param iov_len   Number of elements in @p iov.
     * @param offset    Byte offset within the file.
     * @param bytesRead If not null, receives the number of bytes read on success.
     * @param future    Completion handle; wait() returns 0 on success or a errno on failure.
     */
    static void read(int fd, iovec * iov, uint64_t iov_len, uint64_t offset, uint64_t * bytesRead, IoFuture * future) noexcept;

    /**
     * Blocking write: submit a write from @p buf and suspend the calling fiber
     * until the IO completes.
     *
     * @param fd           File descriptor to write to.
     * @param buf          Source buffer.
     * @param len          Number of bytes to write.
     * @param offset       Byte offset within the file.
     * @param bytesWritten If not null, receives the number of bytes written on success.
     * @return             0 on success, or a errno on failure.
     */
    static int write(int fd, const void * buf, uint64_t len, uint64_t offset, uint64_t * bytesWritten = nullptr) noexcept
    {
        IoFuture future;
        iovec iov{const_cast<void *>(buf), len};
        write(fd, &iov, 1, offset, bytesWritten, &future);
        return future.wait();
    }

    /**
     * Async gather write. Returns immediately; the caller must wait on @p future
     * for the result.
     *
     * @param fd           File descriptor to write to.
     * @param iov          Array of buffers (gather list).
     * @param iov_len      Number of elements in @p iov.
     * @param offset       Byte offset within the file.
     * @param bytesWritten If not null, receives the number of bytes written on success.
     * @param future       Completion handle; wait() returns 0 on success or a errno on failure.
     */
    static void write(int fd, iovec * iov, uint64_t iov_len, uint64_t offset, uint64_t * bytesWritten, IoFuture * future) noexcept;

    /**
     * Blocking poll: suspend the calling fiber until one of the requested
     * events becomes ready on @p fd.
     *
     * @param fd              File descriptor to poll.
     * @param events          Requested events bitmask (e.g. POLLIN, POLLOUT).
     * @param triggeredEvents If not null, receives the triggered events bitmask on success.
     * @return                0 on success, or a errno on failure.
     */
    static int poll(int fd, uint32_t events, uint64_t * triggeredEvents = nullptr) noexcept
    {
        IoFuture future;
        poll(fd, events, triggeredEvents, &future);
        return future.wait();
    }

    /**
     * Async poll. Returns immediately; the caller must wait on @p future for
     * the result.
     *
     * @param fd              File descriptor to poll.
     * @param events          Requested events bitmask (e.g. POLLIN, POLLOUT).
     * @param triggeredEvents If not null, receives the triggered events bitmask on success.
     * @param future          Completion handle; wait() returns 0 on success or a errno on failure.
     */
    static void poll(int fd, uint32_t events, uint64_t * triggeredEvents, IoFuture * future) noexcept;

    /**
     * Completion handle for an async sleep submitted via sleep().
     */
    class SleepFuture : public FiberFuture
    {
    public:
        /**
         * Request cancellation of the pending sleep.
         * Returns immediately; the future will complete with ECANCELED
         * (or with 0 if the sleep expired before cancellation was processed).
         */
        void cancel() noexcept { FiberScheduler::cancelSleep(this); }

    private:
        friend class FiberScheduler;

        static constexpr uint32_t IN_TABLE = 1 << 0;
        static constexpr uint32_t CANCELLED = 1 << 1;

        StackEntry stackEntry;
        TreeEntry treeEntry;
        uint64_t deadlineCycles = 0;
        uint32_t processorNumber = UINT32_MAX;
        std::atomic<uint32_t> state{};
    };

    /**
     * Blocking sleep: suspend the calling fiber for at least @p nanoseconds.
     */
    static void sleep(uint64_t nanoseconds) noexcept
    {
        SleepFuture future;
        sleep(nanoseconds, &future);
        future.wait();
    }

    /**
     * Async sleep. Returns immediately; the caller must wait on @p future
     * for the result. The future completes with 0 on normal expiry or
     * ECANCELED if future->cancel() is called before the deadline.
     *
     * @param nanoseconds  Minimum sleep duration.
     * @param future       Completion handle.
     */
    static void sleep(uint64_t nanoseconds, SleepFuture * future) noexcept;

    /**
     * Return latency percentiles for the given event kind and fiber category,
     * aggregated across all per-CPU profilers.
     * @param kind      Profile event kind.
     * @param category  Fiber category (FiberId::category).
     */
    static LatencyReport reportLatency(ProfileEventKind kind, uint8_t category) noexcept;

private:
    struct SchedulerState;
    struct ProcessorState;
    struct CpuTimer;

    struct CompareDeadline
    {
        bool operator()(const SleepFuture & l, const SleepFuture & r) const noexcept { return l.deadlineCycles < r.deadlineCycles; }
    };

    using SleepStack = LockFreeStack<SleepFuture, &SleepFuture::stackEntry>;
    using SleepTree = Tree<SleepFuture, &SleepFuture::treeEntry, CompareDeadline, true /* AllowDuplicates */>;

    struct StealCandidate
    {
        uint32_t processorNumber;
        uint64_t costCycles;
    };

    struct CompareStealCost
    {
        bool operator()(const StealCandidate & l, const StealCandidate & r) const noexcept
        {
            if (l.costCycles != r.costCycles)
            {
                return l.costCycles < r.costCycles;
            }
            return l.processorNumber < r.processorNumber;
        }
    };

    //
    // Helpers.
    //

    template <typename T>
    static void destroyParameters(void * p) noexcept
    {
        std::destroy_at(static_cast<T *>(p));
    }

    static void buildStealCandidates() noexcept;
    static void * getFiberParameters(Fiber * fiber) noexcept;
    static Fiber * allocateFiber(FiberMain * fiberMain, ParametersDtor * parametersDtor, uint8_t category, FiberFuture * future) noexcept;
    static void freeFiber(Fiber * fiber) noexcept;
    static void enqueueReady(Fiber * fiber) noexcept;
    static void yieldSuspendCallback(Fiber * fiber, void * context) noexcept;
    static void enterThreadModeSuspendCallback(Fiber * fiber, void * context) noexcept;
    static void exitThreadModeSuspendCallback(Fiber * fiber, void * context) noexcept;
    template <typename Setup>
    static void enqueueIo(IoFuture * future, Setup && setup) noexcept;
    static void cancelIo(IoFuture * future) noexcept;
    static void cancelSleep(SleepFuture * future) noexcept;
    static void runScheduler(ProcessorState * processor) noexcept;
    static bool runServiceLoop(ProcessorState * processor, uint64_t waitNs, CpuTimer * timer) noexcept;
    static bool runStealLoop(ProcessorState * processor, uint64_t idleSinceCycles, CpuTimer * timer) noexcept;
    static bool handleReadyQueue(ProcessorState * processor, CpuTimer * timer) noexcept;
    static bool handleCompletionQueue(ProcessorState * processor) noexcept;
    static bool handleSleepQueue(ProcessorState * processor) noexcept;
    static bool handleCancelQueue(ProcessorState * processor) noexcept;
    static bool handleExpiredWaiters(ProcessorState * processor) noexcept;
    static void runFiber(Fiber * fiber, CpuTimer * timer) noexcept;
    static void runThreadWorker() noexcept;

    //
    // State.
    //

    static Options options;
    static SchedulerState * scheduler;
};

} // namespace silk
