#include <silk/fibers/fiber.h>

#include "cpu.h"

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

#include <boost/context/detail/fcontext.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

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

//
// Constants.
//

static constexpr uint64_t FIBER_STACK_SIZE = 64 * 1024;
static constexpr uint64_t READY_QUEUE_CAPACITY = 1024;
static constexpr uint64_t IO_URING_QUEUE_SIZE = 128;
static constexpr uint64_t WAITER_TABLE_SIZE = 4096;
static constexpr uint64_t INITIAL_WAIT_NS = 1'000;
static constexpr uint64_t MAX_WAIT_NS = 10'000'000;
static constexpr uint64_t SPIN_THRESHOLD_NS = 20'000;
static constexpr uint64_t CQE_TAG_CANCEL = 0;
static constexpr uint64_t CQE_TAG_TIMEOUT = 1;
static constexpr uint64_t CQE_TAG_DOORBELL = 2;

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
    x(SCHEDULER_IDLE_TIME,     "SchedulerIdleTime")
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

    bool initialize(FiberMain * fiberMain, ParametersDtor * parametersDtor, FiberFuture * waitingFuture) noexcept;
    void deinitialize() noexcept;

    void switchToFiberContext() noexcept;
    void switchToThreadContext(bool final) noexcept;

    void changeState(FiberState expectedState, FiberState newState) noexcept;
    bool tryChangeStateToSuspended() noexcept;
    bool tryChangeStateToReady() noexcept;

    void wakeThread() noexcept;
    void parkThread() noexcept;

    // Fiber entry point.  Called once when the fiber is first activated.
    static void fiberContextMain(boost::context::detail::transfer_t transfer) noexcept;

    // Cache line 0: scheduling hot path.
    struct alignas(CACHELINE_SIZE)
    {
        // Intrusive node for pool free-list and WaitStack membership.
        StackEntry stackEntry;

        // Lifecycle state. Transitions are performed via CAS (tryChangeState) or
        // unconditional exchange (changeState) to coordinate scheduler and waiters.
        std::atomic<FiberState> state;

        // True for proxy fibers created by getCurrentFiber() on non-fiber threads.
        // These use semaphores rather than context switching in suspend()/schedule().
        bool isProxyFiber = false;

        // True while the fiber is running on the thread worker pool.
        bool inThreadMode = false;

        // CPU this fiber is assigned to; UINT32_MAX means unassigned.
        uint32_t processorNumber = UINT32_MAX;

        // mmap'd stack and Boost.Context fcontext handles for cooperative switching.
        void * stack = nullptr;
        boost::context::detail::fcontext_t fiberContext = nullptr;
        boost::context::detail::fcontext_t threadContext = nullptr;

        // Suspended-list membership; linked while the fiber is SUSPENDED.
        ListEntry suspendedEntry;

        // Node ready for the next enqueue to the shared ready queue.
        QueueBase::QueueNode * reservedNode = nullptr;
    };

    // Cache line 1: suspend/join state.
    struct alignas(CACHELINE_SIZE)
    {
        // Suspend callback set by suspend(), invoked by runFiber() after the
        // context switch back to the scheduler or thread worker.
        FiberScheduler::SuspendCallback * suspendCallback = nullptr;
        void * suspendContext = nullptr;

        // Processor whose suspendedList this fiber is currently in;
        // UINT32_MAX when not linked.
        uint32_t suspendedProcessorNumber = UINT32_MAX;

        // Return value of fiberMain; valid after the fiber reaches STOPPED state.
        int result = 0;

        // Set by run() to the FiberFuture to notify on completion.
        FiberFuture * waitingFuture = nullptr;

        // Used by proxy fibers for blocking in suspend() and unblocking in schedule().
        sem_t threadSemaphore{};
    };

    // Embedded node for the shared ready queue. Its memory is always valid
    // because fiberPool never frees. reservedNode points to whichever node
    // is available for the next enqueue; after each dequeue the recycled
    // dummy is stored here so it can be reused.
    QueueBase::QueueNode queueNode;

    // Entry point, parameters buffer, and optional parameters destructor.
    // parametersDtor is set by run<T> for non-trivially-destructible T and
    // called by fiberContextMain immediately after fiberMain returns.
    FiberMain * fiberMain = nullptr;
    ParametersDtor * parametersDtor = nullptr;
    uint8_t parameters[FIBER_PARAMETERS_SIZE];

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
        ASSERT(!r);
    }
}

Fiber::~Fiber() noexcept
{
    if (isProxyFiber)
    {
        int r = ::sem_destroy(&threadSemaphore);
        ASSERT(!r);
    }

    if (stack)
    {
        int r = ::munmap(stack, FIBER_STACK_SIZE + 2 * PAGE_SIZE);
        ASSERT(!r);
    }
}

bool Fiber::initialize(FiberMain * fiberMain_, ParametersDtor * parametersDtor_, FiberFuture * waitingFuture_) noexcept
{
    state.store(FiberState::SUSPENDED, std::memory_order_relaxed);

    inThreadMode = false;
    processorNumber = UINT32_MAX;
    suspendCallback = nullptr;
    suspendContext = nullptr;
    suspendedProcessorNumber = UINT32_MAX;
    result = 0;
    waitingFuture = waitingFuture_;

    if (!stack)
    {
        stack = ::mmap(nullptr, FIBER_STACK_SIZE + 2 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack == MAP_FAILED) [[unlikely]]
        {
            stack = nullptr;
            return false;
        }

        int r = ::mprotect(stack, PAGE_SIZE, PROT_NONE);
        ASSERT(!r);

        r = ::mprotect(static_cast<uint8_t *>(stack) + PAGE_SIZE + FIBER_STACK_SIZE, PAGE_SIZE, PROT_NONE);
        ASSERT(!r);
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
    fiberContext = boost::context::detail::make_fcontext(
        static_cast<uint8_t *>(stack) + PAGE_SIZE + FIBER_STACK_SIZE, FIBER_STACK_SIZE, fiberContextMain);

    return true;
}

void Fiber::deinitialize() noexcept
{
    ASSERT(!suspendedEntry.is_linked());

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
    __sanitizer_start_switch_fiber(&schedulerFakeStack, static_cast<uint8_t *>(stack) + PAGE_SIZE, FIBER_STACK_SIZE);
#endif

#if defined(__SANITIZE_THREAD__)
    tsanSchedulerFiber = TSAN_FIBER_GET_CURRENT();
    TSAN_FIBER_SWITCH(tsanFiber);
#endif

    auto transfer = boost::context::detail::jump_fcontext(fiberContext, this);
    fiberContext = transfer.fctx;

#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_finish_switch_fiber(schedulerFakeStack, nullptr, nullptr);
#endif
}

void Fiber::switchToThreadContext(bool final) noexcept
{
    UNUSED(final);

#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_start_switch_fiber(final ? nullptr : &asanFakeStack, asanSchedulerStackBottom, asanSchedulerStackSize);
#endif

#if defined(__SANITIZE_THREAD__)
    TSAN_FIBER_SWITCH(tsanSchedulerFiber);
#endif

    auto transfer = boost::context::detail::jump_fcontext(threadContext, nullptr);
    threadContext = transfer.fctx;

#if defined(__SANITIZE_ADDRESS__)
    // Only reached on resume (final=false path); final=true never returns here.
    // Recapture scheduler stack bounds in case fiber migrated to a different thread.
    __sanitizer_finish_switch_fiber(asanFakeStack, &asanSchedulerStackBottom, &asanSchedulerStackSize);
#endif
}

void Fiber::fiberContextMain(boost::context::detail::transfer_t transfer) noexcept
{
    // transfer is populated by Boost.Context's uninstrumented assembly code.
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
    ASSERT(false, "unreachable");
}

void Fiber::changeState(FiberState expectedState, FiberState newState) noexcept
{
    FiberState prevState = state.exchange(newState, std::memory_order_acq_rel);
    ASSERT(
        prevState == expectedState,
        "invalid fiber state: expected={}, actual={}",
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
                ASSERT(false, "Unexpected fiber state: {}", static_cast<int>(currentState));
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
                ASSERT(false, "Unexpected fiber state: {}", static_cast<int>(currentState));
        }
    }
}

void Fiber::wakeThread() noexcept
{
    Perf::getSimpleCounter(simpleCounters[PROXY_FIBER_WAKED]).increment();

    int r = ::sem_post(&threadSemaphore);
    ASSERT(!r);
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
            ASSERT(r == EINTR);
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

    void wakeThread() noexcept;
    void parkThread(uint64_t waitNs, CpuTimer * timer) noexcept;

    bool hasWork() const noexcept;
    void enqueueWakeup() noexcept;

    template <typename Setup>
    bool enqueueIo(IoFuture * future, Setup && setup) noexcept;
    void submitIo() noexcept;

    void insertSuspended(Fiber * fiber) noexcept;
    void removeSuspended(Fiber * fiber) noexcept;

    // Cache line 0: scheduling hot path.
    struct alignas(CACHELINE_SIZE)
    {
        // Deadline of the in-flight io_uring timeout SQE; 0 when none is pending.
        uint64_t wakeupDeadlineCycles = 0;

        // Neighboring CPUs sorted by estimated steal cost (topology-aware).
        std::unique_ptr<StealCandidate[]> stealCandidates;

        // CPU index this processor is pinned to; UINT32_MAX means inactive.
        uint32_t number = UINT32_MAX;

        // eventfd used as a wakeup doorbell.  wakeThread() writes to it;
        // a persistent IORING_OP_POLL_ADD_MULTI SQE delivers a CQE to the
        // ring each time it becomes readable, waking io_uring_enter2.
        int eventFd = -1;

        // Set to true by runScheduler after initialization completes.
        // The steal loop checks this before accessing the ring,
        // and FiberScheduler spins on it before spawning worker threads.
        std::atomic<bool> initialized{};

        // Set just before entering io_uring_enter2; cleared on exit.
        // wakeThread() checks this before writing to eventFd so that
        // eventfd_write is only called when the thread is actually parked.
        std::atomic<bool> sleeping{};

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
    };

    // Cache lines 1-3: ready queue.
    struct alignas(CACHELINE_SIZE)
    {
        BoundedQueue<Fiber *> readyQueue{READY_QUEUE_CAPACITY};
    };

    // Cache line 4+: io_uring ring and service loop state.
    struct alignas(CACHELINE_SIZE)
    {
        // io_uring ring for async IO and sleep timeouts.
        io_uring ring{};

        // sleepQueue/cancelQueue: lock-free stacks drained by the service loop.
        // sleepTree: deadline-ordered set of in-flight sleeps.
        SleepStack sleepQueue;
        SleepStack cancelQueue;
        SleepTree sleepTree;

        // Must be a member: the SQPOLL thread reads the pointer from the SQE
        // asynchronously after enqueueWakeup() returns.
        __kernel_timespec wakeupTs{};
    };
};

void FiberScheduler::ProcessorState::initialize(uint32_t cpu) noexcept
{
    number = cpu;

    eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT(eventFd >= 0);

    io_uring_params params{};
    int r = ::io_uring_queue_init_params(IO_URING_QUEUE_SIZE, &ring, &params);
    ASSERT(!r);

    ASSERT(params.features & IORING_FEAT_NODROP);

    // Submit a persistent poll for the wakeup fd.  IORING_POLL_ADD_MULTI keeps
    // the SQE active indefinitely; each eventfd_write produces one CQE.
    io_uring_sqe * sqe = ::io_uring_get_sqe(&ring);
    ASSERT(sqe);

    ::io_uring_prep_poll_multishot(sqe, eventFd, POLLIN);
    ::io_uring_sqe_set_data64(sqe, CQE_TAG_DOORBELL);

    r = ::io_uring_submit(&ring);
    ASSERT(r >= 0);
}

void FiberScheduler::ProcessorState::destroy() noexcept
{
    if (eventFd >= 0)
    {
        ::io_uring_queue_exit(&ring);
        ::close(eventFd);
    }
}

void FiberScheduler::ProcessorState::wakeThread() noexcept
{
    if (sleeping.load(std::memory_order_acquire))
    {
        Perf::getSimpleCounter(simpleCounters[SCHEDULER_THREAD_WAKED], number).increment();

        int r = ::eventfd_write(eventFd, 1);
        ASSERT(!r);
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
            ASSERT(-r == ETIME || -r == EINTR);
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
        submitIo();
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

            // TSan needs an explicit barrier between submission/completion.
            TSAN_RELEASE(future);
        }

        return true;
    }

    return false;
}

void FiberScheduler::ProcessorState::submitIo() noexcept
{
    // Fast path: submitIo is always called on the same thread that just called
    // enqueueIo, so our own enqueue is always visible here.
    // Suppress the TSan report for the cross-CPU race on sqe_tail.
    TSAN_IGNORE_BEGIN();
    uint32_t count = ::io_uring_sq_ready(&ring);
    TSAN_IGNORE_END();

    if (!count)
    {
        return;
    }

    std::lock_guard lock(submissionLock);

    count = ::io_uring_sq_ready(&ring);
    if (count)
    {
        // TSan needs an explicit barrier between submission/completion.
        TSAN_RELEASE(this);

        int r = ::io_uring_submit(&ring);
        ASSERT(r >= 0);

        Perf::getSimpleCounter(simpleCounters[IO_ENQUEUED], number).increment(count);
    }
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

    WaitStack waiterTable[WAITER_TABLE_SIZE];
};

FiberScheduler::SchedulerState::SchedulerState() noexcept
{
    int r = ::sem_init(&threadSemaphore, 0, 0);
    ASSERT(!r);
}

FiberScheduler::SchedulerState::~SchedulerState() noexcept
{
    int r = ::sem_destroy(&threadSemaphore);
    ASSERT(!r);
}

void FiberScheduler::SchedulerState::wakeThread() noexcept
{
    Perf::getSimpleCounter(simpleCounters[THREAD_WORKER_WAKED]).increment();

    int r = ::sem_post(&threadSemaphore);
    ASSERT(!r);
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
            ASSERT(r == EINTR);
            continue;
        }
        break;
    }
}

void FiberScheduler::initialize() noexcept
{
    ASSERT(!scheduler);

    REGISTER_SIMPLE_COUNTERS(&simpleCounters, FIBER_SIMPLE_COUNTERS);

    scheduler = new SchedulerState();

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
        if (processor->number == UINT32_MAX)
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
            if (scheduler->processorState[other].number != UINT32_MAX)
            {
                cost = topologyCostCycles(topologies[cpu], topologies[other]);
            }
            processor->stealCandidates[i++] = {other, cost};
        }

        std::sort(processor->stealCandidates.get(), processor->stealCandidates.get() + candidateCount, CompareStealCost{});

        // Shuffle CPUs in the same cost group.
        std::mt19937 rng(cpu);
        for (uint32_t start = 0; start < candidateCount;)
        {
            uint64_t groupCost = processor->stealCandidates[start].costCycles;
            uint32_t end = start;
            while (end < candidateCount && processor->stealCandidates[end].costCycles == groupCost)
            {
                ++end;
            }
            std::shuffle(processor->stealCandidates.get() + start, processor->stealCandidates.get() + end, rng);
            start = end;
        }
    }
}

void FiberScheduler::destroy() noexcept
{
    ASSERT(scheduler);

    scheduler->stopping.store(true, std::memory_order_release);

    for (uint32_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        if (processor->number != UINT32_MAX)
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

Fiber * FiberScheduler::getCurrentFiber() noexcept
{
    if (threadFiber)
    {
        return threadFiber;
    }
    if (!proxyFiber) [[unlikely]]
    {
        proxyFiber = std::make_unique<Fiber>(true);
    }
    return proxyFiber.get();
}

bool FiberScheduler::isFiberRunning(Fiber * fiber) noexcept
{
    return fiber->state.load(std::memory_order_acquire) == FiberState::RUNNING;
}

void * FiberScheduler::getFiberParameters(Fiber * fiber) noexcept
{
    return fiber->parameters;
}

Fiber * FiberScheduler::allocateFiber(FiberMain * fiberMain, ParametersDtor * parametersDtor, FiberFuture * future) noexcept
{
    Fiber * fiber = scheduler->fiberPool.allocate();
    if (fiber)
    {
        if (fiber->initialize(fiberMain, parametersDtor, future))
        {
            Perf::getSimpleCounter(simpleCounters[FIBER_STARTED]).increment();
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
        if (!fiber->inThreadMode)
        {
            if (fiber->processorNumber == UINT32_MAX)
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
    UNUSED(context);
    schedule(fiber);
}

void FiberScheduler::enterThreadMode() noexcept
{
    suspend(enterThreadModeSuspendCallback, nullptr);
}

void FiberScheduler::enterThreadModeSuspendCallback(Fiber * fiber, void * context) noexcept
{
    UNUSED(context);

    ASSERT(!fiber->inThreadMode);
    fiber->inThreadMode = true;

    schedule(fiber);
}

void FiberScheduler::exitThreadMode() noexcept
{
    suspend(exitThreadModeSuspendCallback, nullptr);
}

void FiberScheduler::exitThreadModeSuspendCallback(Fiber * fiber, void * context) noexcept
{
    UNUSED(context);

    ASSERT(fiber->inThreadMode);
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
    ASSERT(fiberState == FiberState::RUNNING);
}

void FiberScheduler::enqueueWaiter(uint64_t key, Fiber * fiber) noexcept
{
    uint64_t index = std::hash<uint64_t>{}(key) % WAITER_TABLE_SIZE;
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

    uint64_t index = std::hash<uint64_t>{}(key) % WAITER_TABLE_SIZE;
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
        processor->submitIo();
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

void FiberScheduler::cancelIo(IoFuture * future) noexcept
{
    future->result = nullptr;
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
    uint32_t expected = future->state.load(std::memory_order_relaxed);
    for (;;)
    {
        if (expected & SleepFuture::CANCELLED)
        {
            return;
        }
        if (future->state.compare_exchange_weak(
                expected, expected | SleepFuture::CANCELLED, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            break;
        }
    }

    if (expected & SleepFuture::IN_TABLE)
    {
        ProcessorState * processor = &scheduler->processorState[future->processorNumber];
        processor->cancelQueue.push(future);
        processor->wakeThread();
    }
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
        if (!didWork)
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
            waitNs = waitNs ? std::min(waitNs * 2, MAX_WAIT_NS) : INITIAL_WAIT_NS;
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
        // Wait step starts at INITIAL_WAIT_NS and doubles each idle iteration up to MAX_WAIT_NS.
        // A timed wait ensures sleeping CPUs periodically wake to steal work even when
        // not explicitly signalled (e.g. work arrived on a neighbor via an external thread).
        // Before going to sleep - spin a little to avoid eventfd syscalls.
        if (waitNs < SPIN_THRESHOLD_NS)
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

    return didWork;
}

bool FiberScheduler::handleCompletionQueue(ProcessorState * processor) noexcept
{
    bool didWork = false;

    // TSan needs an explicit barrier between submission/completion.
    TSAN_ACQUIRE(processor);

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
        ASSERT(r >= 0);
    }

    return didWork;
}

bool FiberScheduler::handleSleepQueue(ProcessorState * processor) noexcept
{
    bool didWork = false;

    SleepFuture * sleepFuture = processor->sleepQueue.popAll();
    while (sleepFuture)
    {
        SleepFuture * next = SleepStack::next(sleepFuture);
        uint32_t prev = sleepFuture->state.fetch_or(SleepFuture::IN_TABLE, std::memory_order_acq_rel);
        if (prev & SleepFuture::CANCELLED)
        {
            sleepFuture->state.fetch_and(~SleepFuture::IN_TABLE, std::memory_order_relaxed);
            sleepFuture->set(ECANCELED);
        }
        else
        {
            processor->sleepTree.insert(sleepFuture);
        }
        sleepFuture = next;
        didWork = true;
    }

    return didWork;
}

bool FiberScheduler::handleCancelQueue(ProcessorState * processor) noexcept
{
    bool didWork = false;

    SleepFuture * cancelEntry = processor->cancelQueue.popAll();
    while (cancelEntry)
    {
        SleepFuture * next = SleepStack::next(cancelEntry);
        processor->sleepTree.remove(cancelEntry);
        cancelEntry->state.fetch_and(~SleepFuture::IN_TABLE, std::memory_order_relaxed);
        cancelEntry->set(ECANCELED);
        cancelEntry = next;

        Perf::getSimpleCounter(simpleCounters[SLEEP_CANCELLED], processor->number).increment();
        didWork = true;
    }

    return didWork;
}

bool FiberScheduler::handleExpiredWaiters(ProcessorState * processor) noexcept
{
    bool didWork = false;

    uint64_t now = Tsc::getCycles();
    for (;;)
    {
        SleepFuture * sleepFuture = processor->sleepTree.min();
        if (!sleepFuture || sleepFuture->deadlineCycles > now)
        {
            break;
        }

        processor->sleepTree.remove(sleepFuture);
        sleepFuture->state.fetch_and(~SleepFuture::IN_TABLE, std::memory_order_relaxed);
        sleepFuture->set(0);

        Perf::getSimpleCounter(simpleCounters[SLEEP_EXPIRED], processor->number).increment();
        didWork = true;
    }

    return didWork;
}

void FiberScheduler::runFiber(Fiber * fiber, CpuTimer * timer) noexcept
{
    // Maintain the per-CPU suspended list for GDB debuggability.  suspendedLock
    // and suspendedList are co-located in ProcessorState cache line 0, so these
    // two lock round-trips keep that line warm for the rest of the scheduling
    // hot path.  Benchmarking showed no net cost; the cache warming effect
    // outweighs the lock overhead on uncontested paths.
    if (fiber->suspendedProcessorNumber != UINT32_MAX)
    {
        ProcessorState * processor = &scheduler->processorState[fiber->suspendedProcessorNumber];
        processor->removeSuspended(fiber);
        fiber->suspendedProcessorNumber = UINT32_MAX;
    }

    threadFiber = fiber;

    fiber->changeState(FiberState::READY, FiberState::RUNNING);

    if (timer)
    {
        timer->reset(simpleCounters[SCHEDULER_USER_TIME], fiber->processorNumber);
    }

    fiber->switchToFiberContext();

    if (timer)
    {
        timer->reset(simpleCounters[SCHEDULER_SYSTEM_TIME], fiber->processorNumber);
    }

    threadFiber = nullptr;

    // Submit any SQEs the fiber enqueued during this run.
    ProcessorState * processor = &scheduler->processorState[fiber->processorNumber];
    processor->submitIo();

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
            enqueueReady(fiber);
        }
        return;
    }

    ASSERT(fiberState == FiberState::STOPPED);

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
