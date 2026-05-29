#include <silk/fibers/fiber.h>

#include "cpu.h"
#include "profiler.h"

#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/bounded-queue.h>
#include <silk/util/list.h>
#include <silk/util/memory-pool.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>
#include <silk/util/queue.h>
#include <silk/util/sanitizers.h>
#include <silk/util/spinlock.h>
#include <silk/util/stack.h>
#include <silk/util/tsc.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <fcontext.h>
#include <liburing.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/uio.h>

namespace silk
{

FiberScheduler::Options FiberScheduler::options;
FiberScheduler::SchedulerState * FiberScheduler::scheduler;

//
// Constants.
//

static constexpr uint64_t CQE_TAG_CANCEL = 0;
static constexpr uint64_t CQE_TAG_TIMEOUT = 1;
static constexpr uint64_t CQE_TAG_DOORBELL = 2;

// Hard cap on CPU index (largest known socket: 384 cores).
static constexpr uint16_t INVALID_PROCESSOR_NUMBER = (1 << 10);

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

static Perf::CounterGroup simpleCounters;

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

    bool initialize(FiberId fiberId, FiberMain * fiberMain, ParametersDtor * parametersDtor, FiberFuture * waitingFuture) noexcept;
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
    struct alignas(CACHELINE_SIZE)
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
        uint16_t processorNumber = INVALID_PROCESSOR_NUMBER;

        // Processor whose suspendedList this fiber is currently in.
        uint16_t suspendedProcessorNumber = INVALID_PROCESSOR_NUMBER;

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
    struct alignas(CACHELINE_SIZE)
    {
        // mmap'd stack and fcontext handles for cooperative switching.
        void * stack = nullptr;
        fcontext_t fiberContext = nullptr;
        fcontext_t threadContext = nullptr;

        // Entry point and optional parameters destructor. parametersDtor is set
        // by run for non-trivially-destructible T and called by
        // fiberContextMain immediately after fiberMain returns.
        FiberMain * fiberMain = nullptr;
        ParametersDtor * parametersDtor = nullptr;

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

using WaitStack = LockFreeStack<Fiber, &Fiber::stackEntry>;
using SuspendedList = List<Fiber, &Fiber::suspendedEntry>;

// Current fiber running on this OS thread; null when idle.
static thread_local Fiber * threadFiber = nullptr;

// Proxy fiber for the current non-fiber thread; destroyed at thread exit.
static thread_local std::unique_ptr<Fiber> proxyFiber;

Fiber::Fiber(bool isProxyFiber) noexcept
    : state(isProxyFiber ? FiberState::RUNNING : FiberState::SUSPENDED)
    , isProxyFiber(isProxyFiber)
    , reservedNode(&queueNode)
{
    if (isProxyFiber)
    {
        int r = ::sem_init(&threadSemaphore, 0, 0);
        SILK_ASSERT(!r);
    }
}

Fiber::~Fiber() noexcept
{
    if (isProxyFiber)
    {
        int r = ::sem_destroy(&threadSemaphore);
        SILK_ASSERT(!r);
    }

    if (stack)
    {
        int r = ::munmap(stack, FiberScheduler::getOptions().fiberStackSize + 2 * PAGE_SIZE);
        SILK_ASSERT(!r);
    }
}

bool Fiber::initialize(FiberId fiberId_, FiberMain * fiberMain_, ParametersDtor * parametersDtor_, FiberFuture * waitingFuture_) noexcept
{
    state.store(FiberState::SUSPENDED, std::memory_order_relaxed);

    inThreadMode = false;
    processorNumber = INVALID_PROCESSOR_NUMBER;
    suspendedProcessorNumber = INVALID_PROCESSOR_NUMBER;
    suspendCallback = nullptr;
    suspendContext = nullptr;
    fiberId = fiberId_;
    waitingFuture = waitingFuture_;
    submitTimestamp = 0;
    suspendTimestamp = 0;
    result = 0;

    uint64_t fiberStackSize = FiberScheduler::getOptions().fiberStackSize;

    if (!stack)
    {
        stack = ::mmap(nullptr, fiberStackSize + 2 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack == MAP_FAILED) [[unlikely]]
        {
            stack = nullptr;
            return false;
        }

        int r = ::mprotect(stack, PAGE_SIZE, PROT_NONE);
        SILK_ASSERT(!r);

        r = ::mprotect(static_cast<uint8_t *>(stack) + PAGE_SIZE + fiberStackSize, PAGE_SIZE, PROT_NONE);
        SILK_ASSERT(!r);
    }

#if defined(__SANITIZE_ADDRESS__)
    asanFakeStack = nullptr;
    asanSchedulerStackBottom = nullptr;
    asanSchedulerStackSize = 0;
#endif

#if defined(__SANITIZE_THREAD__)
    tsanFiber = TSAN_FIBER_CREATE();
#endif

    fiberMain = fiberMain_;
    parametersDtor = parametersDtor_;
    fiberContext = make_fcontext(static_cast<uint8_t *>(stack) + PAGE_SIZE + fiberStackSize, fiberStackSize, fiberContextMain);

    return true;
}

void Fiber::deinitialize() noexcept
{
    SILK_ASSERT(!suspendedEntry.is_linked());

    if (parametersDtor)
    {
        parametersDtor(parameters);
    }

#if defined(__SANITIZE_THREAD__)
    TSAN_FIBER_DESTROY(tsanFiber);
    tsanFiber = nullptr;
#endif
}

void Fiber::switchToFiberContext() noexcept
{
#if defined(__SANITIZE_ADDRESS__)
    void * schedulerFakeStack = nullptr;
    __sanitizer_start_switch_fiber(
        &schedulerFakeStack, static_cast<uint8_t *>(stack) + PAGE_SIZE, FiberScheduler::getOptions().fiberStackSize);
#endif

#if defined(__SANITIZE_THREAD__)
    tsanSchedulerFiber = TSAN_FIBER_GET_CURRENT();
    TSAN_FIBER_SWITCH(tsanFiber);
#endif

    auto transfer = jump_fcontext(fiberContext, this);
    fiberContext = transfer.fctx;

#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_finish_switch_fiber(schedulerFakeStack, nullptr, nullptr);
#endif
}

void Fiber::switchToThreadContext(bool final) noexcept
{
    SILK_UNUSED(final);

#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_start_switch_fiber(final ? nullptr : &asanFakeStack, asanSchedulerStackBottom, asanSchedulerStackSize);
#endif

#if defined(__SANITIZE_THREAD__)
    TSAN_FIBER_SWITCH(tsanSchedulerFiber);
#endif

    auto transfer = jump_fcontext(threadContext, nullptr);
    threadContext = transfer.fctx;

#if defined(__SANITIZE_ADDRESS__)
    // Only reached on resume (final=false path); final=true never returns here.
    // Recapture scheduler stack bounds in case fiber migrated to a different thread.
    __sanitizer_finish_switch_fiber(asanFakeStack, &asanSchedulerStackBottom, &asanSchedulerStackSize);
#endif
}

void Fiber::fiberContextMain(transfer_t transfer) noexcept
{
    // transfer is populated by uninstrumented assembly code (jump_fcontext).
    // MSan cannot see those writes, so mark the struct as initialized here.
    MSAN_UNPOISON(&transfer, sizeof(transfer));

    Fiber * fiber = static_cast<Fiber *>(transfer.data);
    fiber->threadContext = transfer.fctx;

#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_finish_switch_fiber(nullptr, &fiber->asanSchedulerStackBottom, &fiber->asanSchedulerStackSize);
#endif

    fiber->result = fiber->fiberMain(fiber->parameters);
    fiber->changeState(FiberState::RUNNING, FiberState::STOPPED);
    fiber->switchToThreadContext(true);
    SILK_FAIL("unreachable");
}

void Fiber::changeState(FiberState expectedState, FiberState newState) noexcept
{
    FiberState prevState = state.exchange(newState, std::memory_order_acq_rel);
    SILK_ASSERT(
        prevState == expectedState,
        "invalid fiber state: expected=%d, actual=%d",
        static_cast<int>(expectedState),
        static_cast<int>(prevState));
}

bool Fiber::tryChangeStateToSuspended() noexcept
{
    FiberState currentState = state.load(std::memory_order_acquire);
    for (;;)
    {
        switch (currentState)
        {
            case FiberState::SUSPEND_REQUESTED:
                if (state.compare_exchange_weak(currentState, FiberState::SUSPENDED, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return true;
                }
                break;
            case FiberState::READY:
                // The suspend callback cancelled the suspension by calling schedule(),
                // which transitioned SUSPEND_REQUESTED -> READY via the cancel path.
                // runFiber will enqueue the fiber after the callback returns.
                return false;
            default:
                SILK_FAIL("unexpected fiber state: %d", static_cast<int>(currentState));
        }
    }
}

bool Fiber::tryChangeStateToReady() noexcept
{
    FiberState currentState = state.load(std::memory_order_acquire);
    for (;;)
    {
        switch (currentState)
        {
            case FiberState::SUSPENDED:
                if (state.compare_exchange_weak(currentState, FiberState::READY, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return true;
                }
                break;
            case FiberState::SUSPEND_REQUESTED:
                // Fiber is mid-callback: cancel by transitioning directly to READY.
                // runFiber will enqueue it after the callback returns.
                if (state.compare_exchange_weak(currentState, FiberState::READY, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return false;
                }
                break;
            default:
                SILK_FAIL("unexpected fiber state: %d", static_cast<int>(currentState));
        }
    }
}

void Fiber::wakeThread() noexcept
{
    Perf::getSimpleCounter(simpleCounters[PROXY_FIBER_WAKED]).increment();

    int r = ::sem_post(&threadSemaphore);
    SILK_ASSERT(!r);
}

void Fiber::parkThread() noexcept
{
    Perf::getSimpleCounter(simpleCounters[PROXY_FIBER_PARKED]).increment();

    for (;;)
    {
        int r = ::sem_wait(&threadSemaphore);
        if (r < 0)
        {
            r = errno;
            SILK_ASSERT(r == EINTR);
            continue;
        }
        break;
    }
}

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

    void reset(uint32_t counter, uint32_t cpu) noexcept
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
    void initialize(uint32_t cpu) noexcept;
    void destroy() noexcept;

    FiberId allocateFiberId(uint8_t category) noexcept;
    void profileEvent(ProfileEventKind kind, uint8_t category, uint64_t durationCycles) noexcept;

    void wakeThread() noexcept;
    void parkThread(uint64_t waitNs, CpuTimer * timer) noexcept;

    bool hasWork() const noexcept;
    void enqueueWakeup() noexcept;

    template <typename Setup>
    bool enqueueIo(IoFuture * future, Setup && setup) noexcept;
    bool submitIo(bool flush) noexcept;
    bool submitIoSlow(uint64_t startCycles) noexcept;

    void insertSuspended(Fiber * fiber) noexcept;
    void removeSuspended(Fiber * fiber) noexcept;

    // Cache line 0: scheduling hot path.
    struct alignas(CACHELINE_SIZE)
    {
        // CPU index this processor is pinned to.
        uint16_t number = INVALID_PROCESSOR_NUMBER;

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

        // Deadline of the in-flight io_uring timeout SQE; 0 when none is pending.
        uint64_t wakeupDeadlineCycles = 0;

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
    };

    // Service-loop state: drained / inspected on every iteration of
    // runServiceLoop.  Laid out adjacent to cache line 0 so the same fiber
    // that just acquired serviceLoopLock pulls these lines in next.
    SleepStack sleepQueue;
    SleepStack cancelQueue;
    SleepTree sleepTree;

    // Per-CPU monotonic counter feeding the counter field of FiberId.
    // Initialized to 1 so the first allocated fiber (cpu=0, counter=0) does
    // not collide with the all-zero sentinel that getCurrentFiberId returns
    // for "no fiber".
    std::atomic<uint64_t> fiberCounter{1};

    // Neighboring CPUs sorted by estimated steal cost (topology-aware).
    // Read only in the steal loop.
    std::unique_ptr<StealCandidate[]> stealCandidates;

    // Timespec pointed at by the in-flight io_uring timeout SQE.  Co-located
    // with the ring because enqueueWakeup writes wakeupTs immediately before
    // it walks the SQ to install the timeout SQE; the kernel's SQPOLL thread
    // reads the pointer asynchronously, so the storage must outlive the call.
    __kernel_timespec wakeupTs{};

    // io_uring ring for async IO and sleep timeouts. Spans 4 cache lines on
    // its own (sq + cq + flags); empty-check on the CQ head is hit on every
    // service-loop iteration.
    io_uring ring{};

    // Per-CPU bounded MPMC queue of ready fibers.  Spans 3 cache lines and
    // is hammered from neighboring CPUs during steal, so left at the end of
    // ProcessorState to keep its cache-line traffic away from the per-CPU
    // hot path on line 0.
    BoundedQueue<Fiber *> readyQueue;
};

void FiberScheduler::ProcessorState::initialize(uint32_t cpu) noexcept
{
    SILK_ASSERT(cpu < INVALID_PROCESSOR_NUMBER);
    number = cpu;

    readyQueue.initialize(options.readyQueueCapacity);

    eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    SILK_ASSERT(eventFd >= 0);

    io_uring_params params{};
    int r = ::io_uring_queue_init_params(options.ioUringQueueSize, &ring, &params);
    SILK_ASSERT(!r);

    SILK_ASSERT(params.features & IORING_FEAT_NODROP);

    // Submit a persistent poll for the wakeup fd.  IORING_POLL_ADD_MULTI keeps
    // the SQE active indefinitely; each eventfd_write produces one CQE.
    io_uring_sqe * sqe = ::io_uring_get_sqe(&ring);
    SILK_ASSERT(sqe);

    ::io_uring_prep_poll_multishot(sqe, eventFd, POLLIN);
    ::io_uring_sqe_set_data64(sqe, CQE_TAG_DOORBELL);

    r = ::io_uring_submit(&ring);
    SILK_ASSERT(r >= 0);

    if (options.enableProfiler)
    {
        profiler = std::make_unique<Profiler>();
    }

    lastCqDrainCycles = Tsc::getCycles();
    lastSubmitCycles.store(lastCqDrainCycles, std::memory_order_relaxed);
}

void FiberScheduler::ProcessorState::destroy() noexcept
{
    if (eventFd >= 0)
    {
        ::io_uring_queue_exit(&ring);
        ::close(eventFd);
    }
}

FiberId FiberScheduler::ProcessorState::allocateFiberId(uint8_t category) noexcept
{
    FiberId fiberId;
    fiberId.counter = fiberCounter.fetch_add(1, std::memory_order_relaxed);
    fiberId.cpu = number;
    fiberId.category = category;
    return fiberId;
}

void FiberScheduler::ProcessorState::profileEvent(ProfileEventKind kind, uint8_t category, uint64_t durationCycles) noexcept
{
    ProfileEvent event;
    event.duration = durationCycles;
    event.category = category;
    event.kind = static_cast<uint8_t>(kind);

    if (!profiler->enqueue(event))
    {
        Perf::getSimpleCounter(simpleCounters[PROFILE_RING_OVERFLOW], number).increment();
    }
}

void FiberScheduler::ProcessorState::wakeThread() noexcept
{
    if (sleeping.load(std::memory_order_acquire))
    {
        Perf::getSimpleCounter(simpleCounters[SCHEDULER_THREAD_WAKED], number).increment();

        int r = ::eventfd_write(eventFd, 1);
        SILK_ASSERT(!r);
    }
}

void FiberScheduler::ProcessorState::parkThread(uint64_t waitNs, CpuTimer * timer) noexcept
{
    __kernel_timespec ts;
    ts.tv_sec = static_cast<int64_t>(waitNs / 1'000'000'000);
    ts.tv_nsec = static_cast<int64_t>(waitNs % 1'000'000'000);

    io_uring_getevents_arg arg{};
    arg.ts = reinterpret_cast<uint64_t>(&ts);

    // Announce that we are about to park.  Release ordering ensures that any
    // concurrent wakeThread() that observes parked=true also observes all prior
    // queue stores, guaranteeing the eventfd_write reaches us.
    sleeping.store(true, std::memory_order_release);

    // Double-check: work may have arrived between the last drain and here.
    // If so, skip the park entirely so that work is not delayed by waitNs.
    if (!hasWork())
    {
        Perf::getSimpleCounter(simpleCounters[SCHEDULER_THREAD_PARKED], number).increment();

        timer->reset(simpleCounters[SCHEDULER_IDLE_TIME], number);

        int r = ::io_uring_enter2(ring.ring_fd, 0, 1, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof(arg));
        if (r < 0)
        {
            // io_uring_enter2 returns -errno directly; it does not set errno.
            // ETIME: timeout expired with no CQE (normal); EINTR: signal interrupted (normal).
            SILK_ASSERT(-r == ETIME || -r == EINTR);
        }

        timer->reset(simpleCounters[SCHEDULER_SYSTEM_TIME], number);
    }

    sleeping.store(false, std::memory_order_relaxed);
}

bool FiberScheduler::ProcessorState::hasWork() const noexcept
{
    if (!readyQueue.empty())
    {
        return true;
    }
    if (!sleepQueue.empty())
    {
        return true;
    }
    if (!cancelQueue.empty())
    {
        return true;
    }

    // Ignore ring->cq.khead atomic/non-atomic mismatch.
    TSAN_IGNORE_BEGIN();
    uint32_t count = ::io_uring_cq_ready(&ring);
    TSAN_IGNORE_END();

    return count > 0;
}

void FiberScheduler::ProcessorState::enqueueWakeup() noexcept
{
    SleepFuture * sleepFuture = sleepTree.min();
    if (!sleepFuture)
    {
        return;
    }

    uint64_t now = Tsc::getCycles();
    if (now >= sleepFuture->deadlineCycles)
    {
        return;
    }

    if (wakeupDeadlineCycles && wakeupDeadlineCycles <= sleepFuture->deadlineCycles)
    {
        return;
    }

    uint64_t remainingNs = Tsc::cyclesToNanoseconds(sleepFuture->deadlineCycles - now);
    wakeupTs.tv_sec = static_cast<int64_t>(remainingNs / 1'000'000'000);
    wakeupTs.tv_nsec = static_cast<int64_t>(remainingNs % 1'000'000'000);

    bool enqueued = enqueueIo(
        nullptr,
        [this](io_uring_sqe * sqe) noexcept
        {
            if (!wakeupDeadlineCycles)
            {
                ::io_uring_prep_timeout(sqe, &wakeupTs, 0, 0);
                ::io_uring_sqe_set_data64(sqe, CQE_TAG_TIMEOUT);
            }
            else
            {
                // A shorter deadline arrived - update the in-flight timeout.
                // CQE_TAG_TIMEOUT identifies the original timeout SQE to update.
                // The update SQE's own CQE will have data=CQE_TAG_CANCEL and is ignored.
                ::io_uring_prep_timeout_update(sqe, &wakeupTs, CQE_TAG_TIMEOUT, 0);
                ::io_uring_sqe_set_data64(sqe, CQE_TAG_CANCEL);
            }
        });

    // If SQ ring full we can just skip this wakeup.
    // The service loop retries enqueueWakeup() on every iteration,
    // so the timeout will be submitted as soon as a slot becomes available.
    if (enqueued)
    {
        wakeupDeadlineCycles = sleepFuture->deadlineCycles;
        submitIo(true);
    }
}

template <typename Setup>
bool FiberScheduler::ProcessorState::enqueueIo(IoFuture * future, Setup && setup) noexcept
{
    std::lock_guard lock(submissionLock);

    io_uring_sqe * sqe = ::io_uring_get_sqe(&ring);
    if (sqe)
    {
        setup(sqe);

        // Contract: when future is non-null, enqueueIo writes it as user_data after
        // setup runs; when future is null, the setup callback is responsible for
        // setting user_data itself (typically to CQE_TAG_CANCEL to ignore the CQE).
        if (future)
        {
            ::io_uring_sqe_set_data(sqe, future);

            if (profiler)
            {
                future->submitTimestamp = Tsc::getCycles();
                future->category = threadFiber ? threadFiber->fiberId.category : 0;
            }

            // TSan needs an explicit barrier between submission/completion.
            TSAN_RELEASE(future);
        }

        return true;
    }

    return false;
}

// Submit pending SQEs to the kernel.  flush=true: unconditional flush.
// flush=false: gated by ioUringFlushThreshold (count) or ioUringFlushTimeout.
bool FiberScheduler::ProcessorState::submitIo(bool flush) noexcept
{
    // Fast path: read SQ tail outside the lock. Returns false without taking
    // the submission lock when there's nothing to submit or the count/staleness
    // thresholds haven't been met. Kept small so it inlines into runFiber and
    // enqueueWakeup; the rest lives in submitIoSlow.
    TSAN_IGNORE_BEGIN();
    uint32_t count = ::io_uring_sq_ready(&ring);
    TSAN_IGNORE_END();
    if (count == 0)
    {
        return false;
    }

    uint64_t nowCycles = Tsc::getCycles();

    if (!flush)
    {
        bool countMet = count >= options.ioUringFlushThreshold;
        bool staleMet = nowCycles - lastSubmitCycles.load(std::memory_order_relaxed) > options.ioUringFlushTimeoutCycles;
        if (!countMet && !staleMet)
        {
            return false;
        }
    }

    return submitIoSlow(nowCycles);
}

__attribute__((noinline)) bool FiberScheduler::ProcessorState::submitIoSlow(uint64_t startCycles) noexcept
{
    std::lock_guard lock(submissionLock);

    uint32_t count = ::io_uring_sq_ready(&ring);
    if (count == 0)
    {
        return false;
    }

    // TSan needs an explicit barrier between submission/completion.
    TSAN_RELEASE(this);

    lastSubmitCycles.store(startCycles, std::memory_order_relaxed);

    int r = ::io_uring_submit(&ring);
    SILK_ASSERT(r >= 0);

    if (profiler)
    {
        profileEvent(ProfileEventKind::SUBMIT_IO, 0, Tsc::getCycles() - startCycles);
    }

    Perf::getSimpleCounter(simpleCounters[IO_ENQUEUED], number).increment(count);
    Perf::getSimpleCounter(simpleCounters[IO_SUBMITTED], number).increment();
    return true;
}

void FiberScheduler::ProcessorState::insertSuspended(Fiber * fiber) noexcept
{
    std::lock_guard lock(suspendedLock);
    suspendedList.push_back(fiber);
}

void FiberScheduler::ProcessorState::removeSuspended(Fiber * fiber) noexcept
{
    std::lock_guard lock(suspendedLock);
    suspendedList.remove(fiber);
}

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

    uint32_t processorCount = 0;
    uint32_t schedulerThreadCount = 0;
    uint32_t workerThreadCount = 0;

    std::unique_ptr<ProcessorState[]> processorState;
    std::unique_ptr<std::thread[]> schedulerThreads;
    std::unique_ptr<std::thread[]> workerThreads;

    MemoryPool<Fiber, &Fiber::stackEntry> fiberPool;
    IntrusiveQueue<Fiber, &Fiber::reservedNode> readyQueue;

    sem_t threadSemaphore{};

    std::unique_ptr<WaitStack[]> waiterTable;
    uint64_t waiterTableMask{};
};

FiberScheduler::SchedulerState::SchedulerState() noexcept
{
    int r = ::sem_init(&threadSemaphore, 0, 0);
    SILK_ASSERT(!r);
}

FiberScheduler::SchedulerState::~SchedulerState() noexcept
{
    int r = ::sem_destroy(&threadSemaphore);
    SILK_ASSERT(!r);
}

void FiberScheduler::SchedulerState::wakeThread() noexcept
{
    Perf::getSimpleCounter(simpleCounters[THREAD_WORKER_WAKED]).increment();

    int r = ::sem_post(&threadSemaphore);
    SILK_ASSERT(!r);
}

void FiberScheduler::SchedulerState::parkThread() noexcept
{
    Perf::getSimpleCounter(simpleCounters[THREAD_WORKER_PARKED]).increment();

    for (;;)
    {
        int r = ::sem_wait(&threadSemaphore);
        if (r < 0)
        {
            r = errno;
            SILK_ASSERT(r == EINTR);
            continue;
        }
        break;
    }
}

void FiberScheduler::initialize(const Options * userOptions) noexcept
{
    SILK_ASSERT(!scheduler);

    REGISTER_SIMPLE_COUNTERS(&simpleCounters, FIBER_SIMPLE_COUNTERS);

    if (userOptions)
    {
        options = *userOptions;
    }

    SILK_ASSERT(options.fiberStackSize >= PAGE_SIZE && (options.fiberStackSize % PAGE_SIZE) == 0);
    SILK_ASSERT(options.readyQueueCapacity >= 2 && (options.readyQueueCapacity & (options.readyQueueCapacity - 1)) == 0);
    SILK_ASSERT(options.ioUringQueueSize >= 2 && (options.ioUringQueueSize & (options.ioUringQueueSize - 1)) == 0);
    SILK_ASSERT(options.ioUringFlushThreshold >= 1 && options.ioUringFlushThreshold <= options.ioUringQueueSize);
    SILK_ASSERT(options.waiterTableSize >= 2 && (options.waiterTableSize & (options.waiterTableSize - 1)) == 0);
    options.ioUringFlushTimeoutCycles = Tsc::nanosecondsToCycles(options.ioUringFlushTimeout);

    scheduler = new SchedulerState();

    scheduler->waiterTable = std::make_unique<WaitStack[]>(options.waiterTableSize);
    scheduler->waiterTableMask = options.waiterTableSize - 1;

    scheduler->processorCount = getProcessorCount();
    scheduler->processorState = std::make_unique<ProcessorState[]>(scheduler->processorCount);

    cpu_set_t processCpuSet;
    CPU_ZERO(&processCpuSet);
    sched_getaffinity(0, sizeof(processCpuSet), &processCpuSet);

    scheduler->schedulerThreadCount = CPU_COUNT(&processCpuSet);
    scheduler->schedulerThreads = std::make_unique<std::thread[]>(scheduler->schedulerThreadCount);

    for (uint32_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        if (CPU_ISSET(cpu, &processCpuSet))
        {
            ProcessorState * processor = &scheduler->processorState[cpu];
            processor->number = cpu;
        }
    }

    buildStealCandidates();

    uint32_t threadIndex = 0;
    for (uint32_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        if (CPU_ISSET(cpu, &processCpuSet))
        {
            ProcessorState * processor = &scheduler->processorState[cpu];
            scheduler->schedulerThreads[threadIndex++] = std::thread(runScheduler, processor);
        }
    }

    for (uint32_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        if (CPU_ISSET(cpu, &processCpuSet))
        {
            ProcessorState * processor = &scheduler->processorState[cpu];
            while (!processor->initialized.load(std::memory_order_acquire))
            {
                cpuPause();
            }
        }
    }

    scheduler->workerThreadCount = scheduler->schedulerThreadCount;
    scheduler->workerThreads = std::make_unique<std::thread[]>(scheduler->workerThreadCount);

    for (uint32_t i = 0; i < scheduler->workerThreadCount; ++i)
    {
        scheduler->workerThreads[i] = std::thread(runThreadWorker);
    }
}

void FiberScheduler::buildStealCandidates() noexcept
{
    auto topologies = std::make_unique<CpuTopology[]>(scheduler->processorCount);
    readCpuTopologies(topologies.get(), scheduler->processorCount);

    uint32_t candidateCount = scheduler->processorCount - 1;
    for (uint32_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        if (processor->number == INVALID_PROCESSOR_NUMBER)
        {
            continue;
        }

        // Build an array of CPUs with the estimated stealing cost.
        processor->stealCandidates = std::make_unique<StealCandidate[]>(candidateCount);

        uint32_t i = 0;
        for (uint32_t other = 0; other < scheduler->processorCount; ++other)
        {
            if (other == cpu)
            {
                continue;
            }
            uint64_t cost = UINT64_MAX;
            if (scheduler->processorState[other].number != INVALID_PROCESSOR_NUMBER)
            {
                cost = topologyCostCycles(topologies[cpu], topologies[other]);
            }
            processor->stealCandidates[i++] = {other, cost};
        }

        std::sort(processor->stealCandidates.get(), processor->stealCandidates.get() + candidateCount, CompareStealCost{});

        // Spread first-choice steal targets within each cost-tie group via a
        // deterministic rotation by cpu % groupSize. Avoids the thundering
        // herd of every CPU racing the same first target while keeping the
        // candidate order reproducible across runs.
        for (uint32_t start = 0; start < candidateCount;)
        {
            uint64_t groupCost = processor->stealCandidates[start].costCycles;
            uint32_t end = start;
            while (end < candidateCount && processor->stealCandidates[end].costCycles == groupCost)
            {
                ++end;
            }
            uint32_t groupSize = end - start;
            if (groupSize > 1)
            {
                uint32_t rotation = cpu % groupSize;
                std::rotate(
                    processor->stealCandidates.get() + start,
                    processor->stealCandidates.get() + start + rotation,
                    processor->stealCandidates.get() + end);
            }
            start = end;
        }
    }
}

void FiberScheduler::destroy() noexcept
{
    SILK_ASSERT(scheduler);

    scheduler->stopping.store(true, std::memory_order_release);

    for (uint32_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        if (processor->number != INVALID_PROCESSOR_NUMBER)
        {
            processor->wakeThread();
        }
    }

    for (uint32_t i = 0; i < scheduler->schedulerThreadCount; ++i)
    {
        scheduler->schedulerThreads[i].join();
    }

    for (uint32_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        processor->destroy();
    }

    for (uint32_t i = 0; i < scheduler->workerThreadCount; ++i)
    {
        scheduler->wakeThread();
    }

    for (uint32_t i = 0; i < scheduler->workerThreadCount; ++i)
    {
        scheduler->workerThreads[i].join();
    }

    delete scheduler;
}

FiberId FiberScheduler::getCurrentFiberId() noexcept
{
    return threadFiber ? threadFiber->fiberId : FiberId{};
}

Fiber * FiberScheduler::getCurrentFiber() noexcept
{
    // Fast path: thread is running a regular fiber, or has already lazily
    // allocated a proxy fiber. Both branches stay inline; only the very first
    // call from a non-fiber thread reaches initCurrentFiber.
    if (threadFiber)
    {
        return threadFiber;
    }
    if (!proxyFiber) [[unlikely]]
    {
        initCurrentFiber();
    }
    return proxyFiber.get();
}

__attribute__((noinline)) void FiberScheduler::initCurrentFiber() noexcept
{
    // Lazily create a proxy fiber so a non-fiber thread can still participate
    // in fiber-aware APIs (e.g. FiberMutex::lock, FiberScheduler::run-and-wait).
    proxyFiber = std::make_unique<Fiber>(true);
}

bool FiberScheduler::isFiberRunning(Fiber * fiber) noexcept
{
    return fiber->state.load(std::memory_order_acquire) == FiberState::RUNNING;
}

void * FiberScheduler::getFiberParameters(Fiber * fiber) noexcept
{
    return fiber->parameters;
}

Fiber *
FiberScheduler::allocateFiber(FiberMain * fiberMain, ParametersDtor * parametersDtor, uint8_t category, FiberFuture * future) noexcept
{
    Fiber * fiber = scheduler->fiberPool.allocate();
    if (fiber)
    {
        ProcessorState * processor = &scheduler->processorState[getCurrentProcessor()];
        FiberId fiberId = processor->allocateFiberId(category);

        if (fiber->initialize(fiberId, fiberMain, parametersDtor, future))
        {
            Perf::getSimpleCounter(simpleCounters[FIBER_STARTED], processor->number).increment();
            return fiber;
        }

        scheduler->fiberPool.deallocate(fiber);
    }

    return nullptr;
}

void FiberScheduler::freeFiber(Fiber * fiber) noexcept
{
    Perf::getSimpleCounter(simpleCounters[FIBER_STOPPED], fiber->processorNumber).increment();

    fiber->deinitialize();
    scheduler->fiberPool.deallocate(fiber);
}

bool FiberScheduler::schedule(Fiber * fiber) noexcept
{
    if (fiber->tryChangeStateToReady())
    {
        enqueueReady(fiber);
        return true;
    }

    return false;
}

void FiberScheduler::enqueueReady(Fiber * fiber) noexcept
{
    if (!fiber->isProxyFiber)
    {
        if (options.enableProfiler)
        {
            fiber->submitTimestamp = Tsc::getCycles();
        }

        if (!fiber->inThreadMode)
        {
            if (fiber->processorNumber == INVALID_PROCESSOR_NUMBER)
            {
                fiber->processorNumber = getCurrentProcessor();
            }

            ProcessorState * processor = &scheduler->processorState[fiber->processorNumber];
            if (processor->readyQueue.enqueue(fiber))
            {
                Perf::getSimpleCounter(simpleCounters[FIBER_ENQUEUED], processor->number).increment();
                processor->wakeThread();
                return;
            }

            // Ready queue full: fall back to worker thread pool.
            Perf::getSimpleCounter(simpleCounters[READY_QUEUE_FULL], processor->number).increment();
        }

        scheduler->readyQueue.enqueue(fiber);

        Perf::getSimpleCounter(simpleCounters[FIBER_ENQUEUED_SHARED]).increment();
        scheduler->wakeThread();
    }
    else
    {
        fiber->wakeThread();
    }
}

void FiberScheduler::yield() noexcept
{
    Fiber * fiber = getCurrentFiber();
    if (!fiber->isProxyFiber)
    {
        suspend(yieldSuspendCallback, nullptr);
    }
}

void FiberScheduler::yieldSuspendCallback(Fiber * fiber, void * context) noexcept
{
    SILK_UNUSED(context);
    schedule(fiber);
}

void FiberScheduler::enterThreadMode() noexcept
{
    suspend(enterThreadModeSuspendCallback, nullptr);
}

void FiberScheduler::enterThreadModeSuspendCallback(Fiber * fiber, void * context) noexcept
{
    SILK_UNUSED(context);

    SILK_ASSERT(!fiber->inThreadMode);
    fiber->inThreadMode = true;

    schedule(fiber);
}

void FiberScheduler::exitThreadMode() noexcept
{
    suspend(exitThreadModeSuspendCallback, nullptr);
}

void FiberScheduler::exitThreadModeSuspendCallback(Fiber * fiber, void * context) noexcept
{
    SILK_UNUSED(context);

    SILK_ASSERT(fiber->inThreadMode);
    fiber->inThreadMode = false;

    schedule(fiber);
}

void FiberScheduler::suspend(SuspendCallback * callback, void * context) noexcept
{
    Fiber * fiber = getCurrentFiber();
    fiber->changeState(FiberState::RUNNING, FiberState::SUSPEND_REQUESTED);

    if (!fiber->isProxyFiber)
    {
        fiber->suspendCallback = callback;
        fiber->suspendContext = context;
        fiber->switchToThreadContext(false);
    }
    else
    {
        if (callback)
        {
            callback(fiber, context);
        }

        if (fiber->tryChangeStateToSuspended())
        {
            fiber->parkThread();
        }
        fiber->changeState(FiberState::READY, FiberState::RUNNING);
    }

    FiberState fiberState = fiber->state.load(std::memory_order_acquire);
    SILK_ASSERT(fiberState == FiberState::RUNNING);
}

void FiberScheduler::enqueueWaiter(uint64_t key, Fiber * fiber) noexcept
{
    uint64_t index = intHash(key) & scheduler->waiterTableMask;
    scheduler->waiterTable[index].push(fiber);

    // seq_cst fence pairs with the one in releaseWaiters to prevent the
    // store-buffering race on weak memory models (e.g. arm64): the caller's
    // subsequent state re-check must observe any concurrent releaseWaiters,
    // and vice versa. See mutex.cpp suspendCallback for the full explanation.
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void FiberScheduler::releaseWaiters(uint64_t key) noexcept
{
    // seq_cst fence pairs with the one in enqueueWaiter.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    uint64_t index = intHash(key) & scheduler->waiterTableMask;
    Fiber * fiber = scheduler->waiterTable[index].popAll();
    while (fiber)
    {
        Fiber * next = WaitStack::next(fiber);
        schedule(fiber);
        fiber = next;
    }
}

template <typename Setup>
void FiberScheduler::enqueueIo(IoFuture * future, Setup && setup) noexcept
{
    ProcessorState * processor;
    for (;;)
    {
        // Re-fetch processor on each iteration: if the SQ ring was full and we
        // yielded, the fiber may have been stolen and now runs on a different CPU.
        processor = &scheduler->processorState[getCurrentProcessor()];
        if (processor->enqueueIo(future, std::forward<Setup>(setup)))
        {
            break;
        }

        // SQ ring full: yield to let the processor drain completions, then retry.
        Perf::getSimpleCounter(simpleCounters[SQ_RING_OVERFLOW], processor->number).increment();
        yield();
    }

    // Regular fiber: runFiber calls submitIo after the fiber suspends.
    // Proxy fiber: submit immediately since there is no runFiber flush.
    Fiber * fiber = getCurrentFiber();
    if (fiber->isProxyFiber)
    {
        processor->submitIo(true);
    }
}

void FiberScheduler::read(int fd, iovec * iov, uint64_t iov_len, uint64_t offset, uint64_t * bytesRead, IoFuture * future) noexcept
{
    future->result = bytesRead;
    enqueueIo(future, [=](io_uring_sqe * sqe) noexcept { ::io_uring_prep_readv(sqe, fd, iov, iov_len, offset); });
}

void FiberScheduler::write(int fd, iovec * iov, uint64_t iov_len, uint64_t offset, uint64_t * bytesWritten, IoFuture * future) noexcept
{
    future->result = bytesWritten;
    enqueueIo(future, [=](io_uring_sqe * sqe) noexcept { ::io_uring_prep_writev(sqe, fd, iov, iov_len, offset); });
}

void FiberScheduler::poll(int fd, uint32_t events, uint64_t * triggeredEvents, IoFuture * future) noexcept
{
    future->result = triggeredEvents;
    enqueueIo(future, [=](io_uring_sqe * sqe) noexcept { ::io_uring_prep_poll_add(sqe, fd, events); });
}

void FiberScheduler::connect(int fd, const sockaddr * addr, socklen_t addrlen, IoFuture * future) noexcept
{
    future->result = nullptr;
    enqueueIo(future, [=](io_uring_sqe * sqe) noexcept { ::io_uring_prep_connect(sqe, fd, addr, addrlen); });
}

void FiberScheduler::accept(int fd, sockaddr * addr, socklen_t * addrlen, int flags, uint64_t * acceptedFd, IoFuture * future) noexcept
{
    future->result = acceptedFd;
    enqueueIo(future, [=](io_uring_sqe * sqe) noexcept { ::io_uring_prep_accept(sqe, fd, addr, addrlen, flags); });
}

void FiberScheduler::cancelIo(IoFuture * future) noexcept
{
    enqueueIo(
        nullptr,
        [=](io_uring_sqe * sqe) noexcept
        {
            ::io_uring_prep_cancel(sqe, future, 0);
            ::io_uring_sqe_set_data64(sqe, CQE_TAG_CANCEL);
        });
}

void FiberScheduler::sleep(uint64_t nanoseconds, SleepFuture * future) noexcept
{
    // Exchange clears state for reuse; if CANCELLED was set before sleep() was
    // called, complete immediately rather than registering in the sleep tree.
    uint32_t prev = future->state.exchange(0, std::memory_order_acq_rel);
    if (prev & SleepFuture::CANCELLED)
    {
        future->set(ECANCELED);
        return;
    }

    future->deadlineCycles = Tsc::getCycles() + Tsc::nanosecondsToCycles(nanoseconds);
    future->processorNumber = getCurrentProcessor();

    ProcessorState * processor = &scheduler->processorState[future->processorNumber];
    processor->sleepQueue.push(future);
    Perf::getSimpleCounter(simpleCounters[SLEEP_ENQUEUED], future->processorNumber).increment();

    processor->wakeThread();
}

void FiberScheduler::cancelSleep(SleepFuture * future) noexcept
{
    uint32_t state = future->state.load(std::memory_order_relaxed);
    for (;;)
    {
        if (state & SleepFuture::CANCELLED)
        {
            return;
        }
        if (future->state.compare_exchange_weak(
                state, state | SleepFuture::CANCELLED, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            break;
        }
    }

    if (state & SleepFuture::IN_TABLE)
    {
        ProcessorState * processor = &scheduler->processorState[future->processorNumber];
        processor->cancelQueue.push(future);
        processor->wakeThread();
    }
}

LatencyReport FiberScheduler::reportLatency(ProfileEventKind kind, uint8_t category) noexcept
{
    Histogram merged;
    for (uint32_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        if (processor->profiler)
        {
            merged.merge(processor->profiler->histogram(kind, category));
        }
    }

    return {
        .p50 = merged.percentile(0.50),
        .p90 = merged.percentile(0.90),
        .p99 = merged.percentile(0.99),
        .p999 = merged.percentile(0.999),
        .count = merged.count(),
    };
}

void FiberScheduler::runScheduler(ProcessorState * processor) noexcept
{
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    CPU_SET(processor->number, &cpuSet);
    ::pthread_setaffinity_np(::pthread_self(), sizeof(cpuSet), &cpuSet);

    // Initialize per-CPU resources pinned to this CPU so that mmap'd memory
    // (io_uring rings, eventfd) is allocated on the local NUMA node.
    processor->initialize(processor->number);
    processor->initialized.store(true, std::memory_order_release);

    uint64_t idleSinceCycles = Tsc::getCycles();
    uint64_t waitNs = 0;

    CpuTimer timer;
    timer.start(simpleCounters[SCHEDULER_SYSTEM_TIME]);

    while (!scheduler->stopping.load(std::memory_order_relaxed))
    {
        // Always run both - handleReadyQueue and runServiceLoop.
        bool didWork = handleReadyQueue(processor, &timer);
        didWork |= runServiceLoop(processor, waitNs, &timer);

        // Steal work only when there is nothing to do on own CPU.
        if (!didWork && !options.disableWorkStealing)
        {
            didWork |= runStealLoop(processor, idleSinceCycles, &timer);
        }

        if (didWork)
        {
            idleSinceCycles = Tsc::getCycles();
            waitNs = 0;
        }
        else
        {
            waitNs = waitNs ? std::min<uint64_t>(waitNs * 2, options.maxWaitNs) : options.initialWaitNs;
        }
    }
}

bool FiberScheduler::runServiceLoop(ProcessorState * processor, uint64_t waitNs, CpuTimer * timer) noexcept
{
    // Only one thread runs the service loop for a given processor at a time.
    if (!processor->serviceLoopLock.try_lock())
    {
        return false;
    }

    if (waitNs)
    {
        // Wait step starts at initialWaitNs and doubles each idle iteration up to maxWaitNs.
        // A timed wait ensures sleeping CPUs periodically wake to steal work even when not
        // explicitly signalled (e.g. work arrived on a neighbor via an external thread).
        // Before going to sleep - spin a little to avoid eventfd syscalls.
        if (waitNs < options.spinThresholdNs)
        {
            spinWait([=] { return processor->hasWork() || scheduler->stopping.load(std::memory_order_relaxed); });
        }
        else
        {
            processor->parkThread(waitNs, timer);
        }
    }

    // Drain all pending work that arrived while we were waiting (or immediately, when waitNs=0).
    bool didWork = false;
    didWork |= handleCompletionQueue(processor);
    didWork |= handleSleepQueue(processor);
    didWork |= handleCancelQueue(processor);
    didWork |= handleExpiredWaiters(processor);

    // Aggregate profile events.
    if (processor->profiler)
    {
        processor->profiler->aggregate();
    }

    // Arm the next io_uring timeout for the earliest pending sleep deadline
    // so parkThread wakes exactly when the next fiber needs to be woken.
    processor->enqueueWakeup();

    processor->serviceLoopLock.unlock();
    return didWork;
}

bool FiberScheduler::runStealLoop(ProcessorState * processor, uint64_t idleSinceCycles, CpuTimer * timer) noexcept
{
    bool didWork = false;

    uint64_t now = Tsc::getCycles();
    // Budget for stealing equals idle time: don't steal from a CPU whose topology
    // cost exceeds how long we have been idle, and don't spend more time stealing
    // than we have already sat idle. A freshly-idle processor skips stealing
    // entirely and lets the backoff in runServiceLoop accumulate idle time first.
    uint64_t idleCycles = now - idleSinceCycles;
    uint64_t deadlineCycles = now + idleCycles;

    uint32_t candidateCount = scheduler->processorCount - 1;
    for (uint32_t i = 0; i < candidateCount && now < deadlineCycles; ++i)
    {
        // Candidates are sorted cheapest first. Once the threshold exceeds our
        // idle duration all remaining candidates are even more expensive.
        StealCandidate * candidate = &processor->stealCandidates[i];
        if (idleCycles < candidate->costCycles)
        {
            break;
        }

        ProcessorState * victim = &scheduler->processorState[candidate->processorNumber];

        // Skip uninitialized processors.
        if (!victim->initialized.load(std::memory_order_acquire))
        {
            continue;
        }

        didWork |= runServiceLoop(victim, 0, timer);

        // We have a limited budget to spend doing work for others.
        bool stoleAny = false;
        for (now = Tsc::getCycles(); now < deadlineCycles; now = Tsc::getCycles())
        {
            Fiber * fiber;
            if (!victim->readyQueue.dequeue(&fiber))
            {
                break;
            }
            Perf::getSimpleCounter(simpleCounters[FIBER_STOLEN], processor->number).increment();

            // Reassign stolen fiber to the current processor.
            fiber->processorNumber = processor->number;
            runFiber(fiber, timer);
            stoleAny = true;
        }

        if (stoleAny)
        {
            // Drain whatever the steal loop left below the pressure-relief
            // threshold before moving on to the next victim.
            processor->submitIo(true);
            didWork = true;
        }
    }

    return didWork;
}

bool FiberScheduler::handleReadyQueue(ProcessorState * processor, CpuTimer * timer) noexcept
{
    bool didWork = false;

    Fiber * fiber;
    while (processor->readyQueue.dequeue(&fiber))
    {
        runFiber(fiber, timer);
        didWork = true;
    }

    // Drain whatever the dispatch left below the pressure-relief threshold
    // so the kernel sees it before the scheduler thread parks.
    if (didWork)
    {
        processor->submitIo(true);
    }

    return didWork;
}

bool FiberScheduler::handleCompletionQueue(ProcessorState * processor) noexcept
{
    // Fast path: CQ ring is empty, nothing to drain.
    if (::io_uring_cq_ready(&processor->ring) == 0)
    {
        return false;
    }

    return handleCompletionQueueSlow(processor);
}

__attribute__((noinline)) bool FiberScheduler::handleCompletionQueueSlow(ProcessorState * processor) noexcept
{
    bool didWork = false;

    // TSan needs an explicit barrier between submission/completion.
    TSAN_ACQUIRE(processor);

    uint64_t entryCycles = 0;
    if (processor->profiler)
    {
        entryCycles = Tsc::getCycles();
    }

    for (;;)
    {
        // Handle completion entries in the ring.
        uint32_t count = 0;

        uint32_t head;
        io_uring_cqe * cqe;
        io_uring_for_each_cqe(&processor->ring, head, cqe)
        {
            ++count;

            uint64_t tag = ::io_uring_cqe_get_data64(cqe);
            if (tag == CQE_TAG_CANCEL)
            {
                // IO cancellation or timeout_update confirmation
                continue;
            }

            if (tag == CQE_TAG_TIMEOUT)
            {
                // Sleep deadline timeout
                processor->wakeupDeadlineCycles = 0;
                continue;
            }

            if (tag == CQE_TAG_DOORBELL)
            {
                // Wakeup doorbell: drain the eventfd counter and keep the
                // POLL_ADD_MULTI SQE active for the next write.
                eventfd_t val;
                ::eventfd_read(processor->eventFd, &val);
                continue;
            }

            // IO completion
            IoFuture * future = reinterpret_cast<IoFuture *>(tag);
            TSAN_ACQUIRE(future);

            Perf::getSimpleCounter(simpleCounters[IO_COMPLETED], processor->number).increment();

            if (processor->profiler)
            {
                uint64_t nowCycles = Tsc::getCycles();
                uint64_t submitCycles = processor->lastSubmitCycles.load(std::memory_order_relaxed);
                processor->profileEvent(ProfileEventKind::IO_WAIT, future->category, nowCycles - future->submitTimestamp);
                processor->profileEvent(ProfileEventKind::SQ_WAIT, future->category, submitCycles - future->submitTimestamp);
            }

            int result = cqe->res;
            if (result >= 0)
            {
                if (future->result)
                {
                    *future->result = static_cast<uint64_t>(result);
                }
                future->set(0);
            }
            else
            {
                future->set(-result);
            }
        }

        if (count > 0)
        {
            ::io_uring_cq_advance(&processor->ring, count);
            didWork = true;
        }

        if (!::io_uring_cq_has_overflow(&processor->ring))
        {
            break;
        }

        Perf::getSimpleCounter(simpleCounters[CQ_RING_OVERFLOW], processor->number).increment();

        // CQ ring overflowed: some CQEs were dropped to the kernel overflow
        // list. Flush them back to the ring so they are processed on the next
        // iteration.
        int r = ::io_uring_get_events(&processor->ring);
        SILK_ASSERT(r >= 0);
    }

    if (didWork && processor->profiler)
    {
        processor->profileEvent(ProfileEventKind::CQ_WAIT, 0, entryCycles - processor->lastCqDrainCycles);
        processor->lastCqDrainCycles = entryCycles;
    }

    return didWork;
}

bool FiberScheduler::handleSleepQueue(ProcessorState * processor) noexcept
{
    // Fast path: queue empty, nothing to do.
    SleepFuture * sleepFuture = processor->sleepQueue.popAll();
    if (!sleepFuture)
    {
        return false;
    }

    handleSleepQueueSlow(processor, sleepFuture);
    return true;
}

__attribute__((noinline)) void FiberScheduler::handleSleepQueueSlow(ProcessorState * processor, SleepFuture * sleepFuture) noexcept
{
    do
    {
        SleepFuture * next = SleepStack::next(sleepFuture);
        uint32_t state = sleepFuture->state.load(std::memory_order_relaxed);
        for (;;)
        {
            if (state & SleepFuture::CANCELLED)
            {
                sleepFuture->set(ECANCELED);
                break;
            }

            SILK_ASSERT(!(state & SleepFuture::IN_TABLE));
            if (sleepFuture->state.compare_exchange_weak(
                    state, state | SleepFuture::IN_TABLE, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                processor->sleepTree.insert(sleepFuture);
                break;
            }
        }
        sleepFuture = next;

    } while (sleepFuture);
}

bool FiberScheduler::handleCancelQueue(ProcessorState * processor) noexcept
{
    // Fast path: queue empty, nothing to do.
    SleepFuture * cancelEntry = processor->cancelQueue.popAll();
    if (!cancelEntry)
    {
        return false;
    }

    handleCancelQueueSlow(processor, cancelEntry);
    return true;
}

__attribute__((noinline)) void FiberScheduler::handleCancelQueueSlow(ProcessorState * processor, SleepFuture * cancelEntry) noexcept
{
    uint64_t count = 0;
    do
    {
        uint32_t prev = cancelEntry->state.fetch_and(~SleepFuture::IN_TABLE, std::memory_order_acq_rel);
        SILK_ASSERT(prev & SleepFuture::IN_TABLE);

        SleepFuture * next = SleepStack::next(cancelEntry);
        processor->sleepTree.remove(cancelEntry);
        cancelEntry->set(ECANCELED);
        cancelEntry = next;
        ++count;

    } while (cancelEntry);

    Perf::getSimpleCounter(simpleCounters[SLEEP_CANCELLED], processor->number).increment(count);
}

bool FiberScheduler::handleExpiredWaiters(ProcessorState * processor) noexcept
{
    // Fast path: tree empty or earliest deadline still in the future. Inlines
    // into runServiceLoop; the expire loop lives in handleExpiredWaitersSlow.
    SleepFuture * sleepFuture = processor->sleepTree.min();
    if (!sleepFuture)
    {
        return false;
    }

    uint64_t now = Tsc::getCycles();
    if (sleepFuture->deadlineCycles > now)
    {
        return false;
    }

    handleExpiredWaitersSlow(processor, sleepFuture, now);
    return true;
}

__attribute__((noinline)) void
FiberScheduler::handleExpiredWaitersSlow(ProcessorState * processor, SleepFuture * sleepFuture, uint64_t now) noexcept
{
    uint64_t count = 0;
    do
    {
        uint32_t state = sleepFuture->state.load(std::memory_order_relaxed);
        for (;;)
        {
            if (state & SleepFuture::CANCELLED)
            {
                sleepFuture = processor->sleepTree.next(sleepFuture);
                break;
            }

            SILK_ASSERT(state & SleepFuture::IN_TABLE);
            if (sleepFuture->state.compare_exchange_weak(
                    state, state & ~SleepFuture::IN_TABLE, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                SleepFuture * next = processor->sleepTree.remove(sleepFuture);
                sleepFuture->set(0);
                sleepFuture = next;
                ++count;
                break;
            }
        }

    } while (sleepFuture && sleepFuture->deadlineCycles <= now);

    Perf::getSimpleCounter(simpleCounters[SLEEP_EXPIRED], processor->number).increment(count);
}

void FiberScheduler::runFiber(Fiber * fiber, CpuTimer * timer) noexcept
{
    fiber->changeState(FiberState::READY, FiberState::RUNNING);

    // Maintain the per-CPU suspended list for GDB debuggability.
    // suspendedLock and suspendedList are co-located in ProcessorState cache line 0.
    // Benchmarking showed no net cost.
    if (fiber->suspendedProcessorNumber != INVALID_PROCESSOR_NUMBER)
    {
        ProcessorState * processor = &scheduler->processorState[fiber->suspendedProcessorNumber];
        processor->removeSuspended(fiber);
        fiber->suspendedProcessorNumber = INVALID_PROCESSOR_NUMBER;
    }

    ProcessorState * processor = &scheduler->processorState[fiber->processorNumber];

    // Only the per-CPU scheduler thread (timer != nullptr) reports profile events:
    // it is pinned and is the sole producer of this CPU's SPSC ring. Worker threads
    // (timer == nullptr) are unpinned and would break the single-producer rule.
    uint64_t runStartCycles = 0;
    if (timer)
    {
        if (processor->profiler)
        {
            runStartCycles = Tsc::getCycles();
            processor->profileEvent(ProfileEventKind::READY_WAIT, fiber->fiberId.category, runStartCycles - fiber->submitTimestamp);

            if (fiber->suspendTimestamp)
            {
                processor->profileEvent(
                    ProfileEventKind::SUSPEND_WAIT, fiber->fiberId.category, fiber->submitTimestamp - fiber->suspendTimestamp);
                fiber->suspendTimestamp = 0;
            }
        }

        timer->reset(simpleCounters[SCHEDULER_USER_TIME], fiber->processorNumber);
    }

    threadFiber = fiber;
    fiber->switchToFiberContext();
    threadFiber = nullptr;

    if (timer)
    {
        if (processor->profiler)
        {
            processor->profileEvent(ProfileEventKind::FIBER_RUN, fiber->fiberId.category, Tsc::getCycles() - runStartCycles);
        }

        timer->reset(simpleCounters[SCHEDULER_SYSTEM_TIME], fiber->processorNumber);
    }

    // Submit any SQEs the fiber enqueued. On the per-CPU scheduler thread
    // (timer != nullptr) use pressure-relief mode so the dispatch loop can
    // amortize the syscall across multiple fibers; on unpinned worker threads
    // there is no batching boundary, so force-submit per fiber.
    processor->submitIo(timer == nullptr);

    FiberState fiberState = fiber->state.load(std::memory_order_acquire);
    if (fiberState == FiberState::SUSPEND_REQUESTED)
    {
        processor->insertSuspended(fiber);
        fiber->suspendedProcessorNumber = fiber->processorNumber;

        SuspendCallback * callback = std::exchange(fiber->suspendCallback, nullptr);
        void * callbackContext = std::exchange(fiber->suspendContext, nullptr);
        if (callback)
        {
            callback(fiber, callbackContext);
        }

        // Stamp the suspend timestamp *before* the CAS so the release on the
        // SUSPENDED transition publishes the write to the eventual waker; if
        // the CAS fails (callback-time cancellation), clear it so the next
        // dispatch doesn't report a phantom SUSPEND_WAIT.
        if (processor->profiler)
        {
            fiber->suspendTimestamp = Tsc::getCycles();
        }

        // Finalize suspension. If schedule cancelled it during the callback
        // (SUSPEND_REQUESTED -> READY), tryChangeStateToSuspended returns false
        // and the fiber is already READY; enqueue it now that the callback has
        // finished and it is safe for the fiber to run.
        if (fiber->tryChangeStateToSuspended())
        {
            Perf::getSimpleCounter(simpleCounters[FIBER_SUSPENDED], processor->number).increment();
        }
        else
        {
            Perf::getSimpleCounter(simpleCounters[FIBER_SUSPEND_CANCELLED], processor->number).increment();
            if (processor->profiler)
            {
                fiber->suspendTimestamp = 0;
            }
            enqueueReady(fiber);
        }
        return;
    }

    SILK_ASSERT(fiberState == FiberState::STOPPED);

    if (fiber->waitingFuture)
    {
        fiber->waitingFuture->set(fiber->result);
    }

    freeFiber(fiber);
}

void FiberScheduler::runThreadWorker() noexcept
{
    while (!scheduler->stopping.load(std::memory_order_relaxed))
    {
        while (Fiber * fiber = scheduler->readyQueue.dequeue())
        {
            runFiber(fiber, nullptr);

            // fiber is either stopped (freed by runFiber) or suspended waiting
            // for an event; in the latter case schedule() will re-enqueue it
            // to readyQueue and post the semaphore when it is ready to run.
        }

        scheduler->parkThread();
    }
}

} // namespace silk
