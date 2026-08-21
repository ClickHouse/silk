#pragma once

#include "profiler.h"

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/bounded-queue.h>
#include <silk/util/list.h>
#include <silk/util/memory-pool.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>
#include <silk/util/queue.h>
#include <silk/util/spinlock.h>
#include <silk/util/stack.h>
#include <silk/util/tsc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include <fcontext.h>
#include <liburing.h>
#include <sched.h>
#include <semaphore.h>

namespace silk
{

//
// Constants.
//

static constexpr uint64_t CQE_TAG_CANCEL = 0;
static constexpr uint64_t CQE_TAG_DOORBELL = 1;
static constexpr uint64_t CQE_TAG_WAKEUP = 2;

// clang-format off
#define FIBER_SIMPLE_COUNTERS(x) \
    x(FIBER_STARTED,           "FiberStarted") \
    x(FIBER_STOPPED,           "FiberStopped") \
    x(FIBER_SUSPENDED,         "FiberSuspended") \
    x(FIBER_SUSPEND_CANCELLED, "FiberSuspendCancelled") \
    x(FIBER_ENQUEUED,          "FiberEnqueued") \
    x(FIBER_ENQUEUED_SHARED,   "FiberEnqueuedShared") \
    x(READY_QUEUE_FULL,        "ReadyQueueFull") \
    x(FIBER_STOLEN,            "FiberStolen") \
    x(IO_ENQUEUED,             "IoEnqueued") \
    x(IO_COMPLETED,            "IoCompleted") \
    x(IO_SUBMITTED,            "IoSubmitted") \
    x(SQ_RING_OVERFLOW,        "SQRingOverflow") \
    x(CQ_RING_OVERFLOW,        "CQRingOverflow") \
    x(SLEEP_ENQUEUED,          "SleepEnqueued") \
    x(SLEEP_EXPIRED,           "SleepExpired") \
    x(SLEEP_CANCELLED,         "SleepCancelled") \
    x(SCHEDULER_THREAD_PARKED, "SchedulerThreadParked") \
    x(SCHEDULER_THREAD_WAKED,  "SchedulerThreadWaked") \
    x(PROXY_FIBER_PARKED,      "ProxyFiberParked") \
    x(PROXY_FIBER_WAKED,       "ProxyFiberWaked") \
    x(THREAD_WORKER_PARKED,    "ThreadWorkerParked") \
    x(THREAD_WORKER_WAKED,     "ThreadWorkerWaked") \
    x(SCHEDULER_USER_TIME,     "SchedulerUserTime") \
    x(SCHEDULER_SYSTEM_TIME,   "SchedulerSystemTime") \
    x(SCHEDULER_IDLE_TIME,     "SchedulerIdleTime") \
    x(PROFILE_RING_OVERFLOW,   "ProfileRingOverflow")
// clang-format on

DECLARE_SIMPLE_COUNTERS(FIBER_SIMPLE_COUNTERS);

// One counter group shared by every fiber-scheduler translation unit; defined in fiber.cpp.
extern Perf::CounterGroup simpleCounters;

// Itanium C++ ABI per-thread exception-propagation state (libstdc++'s __cxxabiv1::__cxa_eh_globals
// and libc++abi's __cxa_eh_globals share this layout): the stack of exceptions being propagated or
// handled on the thread, plus the uncaught count. __cxa_throw / __cxa_begin_catch / __cxa_end_catch
// mutate it and free the exception object through it. It lives in thread-local storage, so every
// fiber on a scheduler thread shares one copy - silk swaps it per fiber on each context switch so an
// exception whose unwind spans a switch is not corrupted by another fiber's exception handling.
struct CxaEhGlobals
{
    void * caughtExceptions = nullptr;
    unsigned int uncaughtExceptions = 0;
};

/**
 * Fiber lifecycle state.
 */
enum class FiberState : uint8_t
{
    // Waiting to be scheduled (initial state, or blocked on suspend()).
    SUSPENDED,
    // Enqueued in a processor's ready queue, waiting for a scheduler thread.
    READY,
    // Currently executing on a scheduler thread.
    RUNNING,
    // Yielded; suspend callback is running; schedule() may cancel instead of enqueue.
    SUSPEND_REQUESTED,
    // Entry point returned; result has been delivered to waitingFuture.
    STOPPED,
};

/**
 * Fiber state: owns a stack and provides services to switch between thread/fiber context.
 * Proxy fibers represent non-fiber threads and block/unblock via a semaphore instead of context switching.
 */
class Fiber
{
public:
    Fiber(bool isProxyFiber = false) noexcept;
    ~Fiber() noexcept;

    bool initialize(FiberId fiberId, FiberMain * fiberMain, FiberParametersDtor * parametersDtor, FiberFuture * waitingFuture) noexcept;
    void deinitialize() noexcept;

    void switchToFiberContext() noexcept;
    void switchToThreadContext(bool final) noexcept;

    void changeState(FiberState expectedState, FiberState newState) noexcept;
    bool tryChangeStateToSuspended() noexcept;
    bool tryChangeStateToReady() noexcept;

    void wakeThread() noexcept;
    void parkThread() noexcept;

    // Fiber entry point.  Called once when the fiber is first activated.
    [[noreturn]] static void fiberContextMain(transfer_t transfer) noexcept;

    // Cache line 0: scheduling + per-suspend hot path. Touched on every
    // dispatch and every suspension. runFiber's full read/write set lives on
    // this single line, so dispatch never pulls a second cache line on the
    // common path.
    struct alignas(kCacheLineSize)
    {
        // Intrusive node for pool free-list and WaitStack membership.
        StackEntry stackEntry;

        // Lifecycle state. Transitions are performed via CAS (tryChangeState) or
        // unconditional exchange (changeState) to coordinate scheduler and waiters.
        std::atomic<FiberState> state;

        // True for proxy fibers created by getCurrentFiber on non-fiber threads.
        // These use semaphores rather than context switching in suspend/schedule.
        bool isProxyFiber = false;

        // True while the fiber is running on the thread worker pool.
        bool inThreadMode = false;

        // CPU this fiber is assigned to.
        uint16_t processorNumber = kInvalidProcessorNumber;

        // Processor whose suspendedList this fiber is currently in.
        uint16_t suspendedProcessorNumber = kInvalidProcessorNumber;

        // Suspend callback set by suspend, invoked by runFiber after the
        // context switch back to the scheduler or thread worker.
        FiberScheduler::SuspendCallback * suspendCallback = nullptr;
        void * suspendContext = nullptr;

        // Suspended-list membership; linked while the fiber is SUSPENDED.
        ListEntry suspendedEntry;

        // Node ready for the next enqueue to the shared ready queue.
        QueueBase::QueueNode * reservedNode = nullptr;

        // Fiber identity.
        FiberId fiberId{};
    };

    // Cache line 1: context-switch state and profiler timestamps; touched on
    // every dispatch. Per-fiber-once fields (fiberMain, parametersDtor,
    // waitingFuture) piggyback for free.
    struct alignas(kCacheLineSize)
    {
        // mmap'd stack and fcontext handles for cooperative switching.
        void * stack = nullptr;
        fcontext_t fiberContext = nullptr;
        fcontext_t threadContext = nullptr;

        // Entry point and optional parameters destructor. parametersDtor is set
        // by run for non-trivially-destructible T and called by
        // fiberContextMain immediately after fiberMain returns.
        FiberMain * fiberMain = nullptr;
        FiberParametersDtor * parametersDtor = nullptr;

        // Set by run to the FiberFuture to notify on completion.
        FiberFuture * waitingFuture = nullptr;

        // TSC timestamp of the most recent work submission (enqueueReady).
        // Read by runFiber to compute the duration for a profile event.
        uint64_t submitTimestamp = 0;

        // TSC timestamp captured when the fiber suspended.  Non-zero between
        // suspend and the next enqueueReady; consumed (and zeroed) when
        // SUSPEND_WAIT is reported on the next dispatch.
        uint64_t suspendTimestamp = 0;
    };

    // Embedded node for the shared ready queue. Its memory is always valid
    // because fiberPool never frees. reservedNode points to whichever node
    // is available for the next enqueue; after each dequeue the recycled
    // dummy is stored here so it can be reused.
    QueueBase::QueueNode queueNode;

    // Proxy-fiber semaphore. Cross-thread sem_post/sem_wait; untouched by
    // regular fibers.
    sem_t threadSemaphore{};

    // Parameters buffer for the fiber's entry point. 8-byte aligned via the
    // preceding sem_t; constructed in-place at fiber start.
    uint8_t parameters[FIBER_PARAMETERS_SIZE];

    // Return value of fiberMain; valid after the fiber reaches STOPPED state.
    int result = 0;

    // This fiber's saved C++ exception-propagation state, swapped with the thread's on each context
    // switch so an exception crossing the switch is not corrupted by another fiber sharing the thread.
    // Placed last so it does not shift the offset-pinned fields above.
    CxaEhGlobals cxaEhGlobals;

#if defined(__SANITIZE_ADDRESS__)
    void * asanFakeStack = nullptr;
    const void * asanSchedulerStackBottom = nullptr;
    size_t asanSchedulerStackSize = 0;
#endif

#if defined(__SANITIZE_THREAD__)
    void * tsanFiber = nullptr;
    void * tsanSchedulerFiber = nullptr;
#endif
};

// FiberFuture::State.waiter is 61 bits; FiberMutex::State.owner is 63 bits.
// Both store Fiber* directly. >=8-byte alignment guarantees the bottom 3 bits
// are always zero, so the full 64-bit address fits in either field without masking.
static_assert(alignof(Fiber) >= 8);

static_assert(offsetof(Fiber, parameters) == FIBER_PARAMETERS_OFFSET);

using WaitStack = LockFreeStack<Fiber, &Fiber::stackEntry>;
using SuspendedList = List<Fiber, &Fiber::suspendedEntry>;

/**
 * Partitions scheduler thread CPU time into named buckets without gaps or
 * double-counting. Call start() once to begin timing the first bucket, then
 * reset() on every transition: it flushes elapsed time into the active counter
 * and begins timing the next one.
 */
struct FiberScheduler::CpuTimer
{
    void start(uint32_t counter) noexcept
    {
        startedCycles = Tsc::getCycles();
        counterRunning = counter;
    }

    void reset(uint32_t counter, uint16_t cpu) noexcept
    {
        uint64_t now = Tsc::getCycles();
        uint64_t elapsedNs = Tsc::cyclesToNanoseconds(now - startedCycles);
        Perf::getSimpleCounter(counterRunning, cpu).increment(elapsedNs);

        startedCycles = now;
        counterRunning = counter;
    }

    uint64_t startedCycles = 0;
    uint32_t counterRunning = 0;
};

/**
 * Per-CPU state: ready queue, io_uring ring, sleep tree, and eventfd for wakeup.
 */
struct FiberScheduler::ProcessorState
{
    void initialize(uint16_t cpu) noexcept;
    void destroy() noexcept;

    FiberId allocateFiberId(uint8_t category) noexcept;
    void profileEvent(ProfileEventKind kind, uint8_t category, uint64_t durationCycles) noexcept;

    void wakeThread() noexcept;
    void parkThread(uint64_t waitNs, CpuTimer * timer) noexcept;
    bool hasWork() const noexcept;
    uint32_t sqReady() const noexcept;
    uint32_t cqReady() const noexcept;

    void enqueueDoorbell() noexcept;
    void postWakeup(ProcessorState * target) noexcept;
    bool enqueueWakeup(ProcessorState * target) noexcept;

    template <typename Setup>
    bool enqueueIo(IoFuture * future, Setup && setup) noexcept;
    bool submitIo(bool flush) noexcept;
    bool submitIoSlow(uint64_t startCycles) noexcept;

    void insertSuspended(Fiber * fiber) noexcept;
    void removeSuspended(Fiber * fiber) noexcept;

    // Cache line 0: scheduling hot path.
    struct alignas(kCacheLineSize)
    {
        // CPU index this processor is pinned to.
        uint16_t number = kInvalidProcessorNumber;

        // Set to true by runScheduler after initialization completes.
        // The steal loop checks this before accessing the ring,
        // and FiberScheduler spins on it before spawning worker threads.
        std::atomic<bool> initialized{};

        // Set just before entering io_uring_enter2; cleared on exit.
        // wakeThread() checks this before writing to eventFd so that
        // eventfd_write is only called when the thread is actually parked.
        std::atomic<bool> sleeping{};

        // eventfd used as a wakeup doorbell.  wakeThread() writes to it;
        // a persistent IORING_OP_POLL_ADD_MULTI SQE delivers a CQE to the
        // ring each time it becomes readable, waking io_uring_enter2.
        int eventFd = -1;

        // Serializes the service loop (CQ draining, sleep insertion/expiry) so
        // that steal loops on neighboring CPUs can assist without races.
        SpinLock serviceLoopLock;

        // Serializes all SQ submissions (io_uring_get_sqe + io_uring_submit).
        // Multiple worker threads can land on the same CPU and call enqueueIo
        // concurrently; io_uring's SQ ring is not thread-safe.
        SpinLock submissionLock;

        // Protects suspendedList; co-located so insert/remove touch only this
        // cache line rather than also pulling in cache line 4+.
        SpinLock suspendedLock;

        // Per-CPU suspended list for GDB observability. Co-located with
        // suspendedLock so that insert/remove touch only this cache line.
        SuspendedList suspendedList;

        // Timestamp (TSC cycles) of the most recent successful CQ drain on
        // this ring. Used to emit CQ_WAIT profile events bounding how long
        // any CQE in the next drain batch could have sat in the ring.
        uint64_t lastCqDrainCycles = 0;

        // Timestamp (TSC cycles) of the most recent io_uring_submit call.
        // Read in submitIo (time-gate) and handleCompletionQueue (SQ_WAIT
        // emit) under serviceLoopLock; written in submitIo under
        // submissionLock.  Relaxed atomic is sufficient: readers tolerate a
        // slightly stale value.
        std::atomic<uint64_t> lastSubmitCycles{0};

        // Per-CPU latency profiler. Allocated only when Options::enableProfiler
        // is set; null otherwise.  Co-located with the hot path so the null
        // check in reportFiberWait/reportIoWait costs no additional miss when
        // profiling is off.  The scheduler thread for this CPU is the sole
        // producer; aggregate is called by the service loop under serviceLoopLock.
        std::unique_ptr<Profiler> profiler;

        // Per-CPU monotonic counter feeding the counter field of FiberId.
        // Initialized to 1 so the first allocated fiber (cpu=0, counter=0) does
        // not collide with the all-zero sentinel that getCurrentFiberId returns
        // for "no fiber".
        std::atomic<uint64_t> fiberCounter{1};
    };

    // Service-loop state: drained / inspected on every iteration of
    // runServiceLoop.  Laid out adjacent to cache line 0 so the same fiber
    // that just acquired serviceLoopLock pulls these lines in next.
    SleepStack sleepQueue;
    SleepStack cancelQueue;
    SleepTree sleepTree;

    // Neighboring CPUs sorted by estimated steal cost (topology-aware).
    // Read only in the steal loop.
    std::unique_ptr<StealCandidate[]> stealCandidates;

    // io_uring ring for async IO. Spans 4 cache lines on
    // its own (sq + cq + flags); empty-check on the CQ head is hit on every
    // service-loop iteration.
    io_uring ring{};

    // Flags for the idle park's io_uring_enter2.
    uint32_t parkEnterFlags;

    // Per-CPU bounded MPMC queue of ready fibers.  Spans 3 cache lines and
    // is hammered from neighboring CPUs during steal, so left at the end of
    // ProcessorState to keep its cache-line traffic away from the per-CPU
    // hot path on line 0.
    BoundedQueue<Fiber *> readyQueue;
};

/**
 * Global scheduler state: processor array, scheduler/worker threads, fiber pool,
 * fallback ready queue for overflow fibers, and waiter table for sync primitives.
 */
struct FiberScheduler::SchedulerState
{
    SchedulerState() noexcept;
    ~SchedulerState() noexcept;

    void wakeThread() noexcept;
    void parkThread() noexcept;

    //
    // State.
    //

    std::atomic<bool> stopping{};

    uint16_t processorCount = 0;
    uint16_t schedulerThreadCount = 0;
    uint16_t workerThreadCount = 0;

    // Maps every configured CPU to the processor a thread running there injects into:
    // an active CPU to its own processor, an inactive CPU (excluded from the
    // active set) to an active processor chosen round-robin, so work injected
    // from a reserved core lands on a real ring instead of an uninitialized one.
    std::unique_ptr<ProcessorState *[]> homeProcessor;

    // The set of active CPUs. Worker threads pin to it so silk never runs
    // fibers on reserved cores.
    cpu_set_t activeMask;

    std::unique_ptr<std::thread[]> schedulerThreads;
    std::unique_ptr<std::thread[]> workerThreads;

    MemoryPool<Fiber, &Fiber::stackEntry> fiberPool;
    IntrusiveQueue<Fiber, &Fiber::reservedNode> readyQueue;

    sem_t threadSemaphore{};

    std::unique_ptr<WaitStack[]> waiterTable;
    uint64_t waiterTableMask{};

    // Declared last so it is destroyed first: each ProcessorState's suspended
    // list links nodes embedded in pool-owned fibers, so the lists must be
    // destroyed before fiberPool frees the fiber memory.
    std::unique_ptr<ProcessorState[]> processorState;
};

} // namespace silk
