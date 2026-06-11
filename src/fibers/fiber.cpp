#include <silk/fibers/fiber.h>

#include "cpu-controller.h"
#include "cpu.h"
#include "fiber.h"
#include "profiler.h"

#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/bitmap.h>
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
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <cxxabi.h>
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

Perf::CounterGroup simpleCounters;

// libstdc++ declares __cxxabiv1::__cxa_get_globals in <cxxabi.h>; libc++abi omits it from its public
// header, so declare the Itanium ABI symbol (matching its internal signature) there.
#if defined(_LIBCPP_VERSION)
namespace __cxxabiv1
{
struct __cxa_eh_globals;
extern "C" __cxa_eh_globals * __cxa_get_globals() noexcept;
}
#endif

// Read the current thread's exception state. Re-fetched on every switch so a migrated fiber reads
// the state of whichever thread it now runs on.
static CxaEhGlobals loadExceptionState() noexcept
{
    return *reinterpret_cast<CxaEhGlobals *>(__cxxabiv1::__cxa_get_globals());
}

static void storeExceptionState(const CxaEhGlobals & state) noexcept
{
    *reinterpret_cast<CxaEhGlobals *>(__cxxabiv1::__cxa_get_globals()) = state;
}

// Current fiber running on this OS thread; null when idle.  External linkage on purpose:
// clang emits no DWARF location for thread-locals on aarch64, and gdb can resolve the TLS
// address of an external symbol through the ELF symbol table - fiber.py reads this variable
// on every thread to list RUNNING fibers.
thread_local Fiber * threadFiber = nullptr;

// Proxy fiber for the current non-fiber thread; destroyed at thread exit.
static thread_local std::unique_ptr<Fiber> proxyFiber;

// Set for the lifetime of a scheduler thread. A scheduler thread must never block
// on the proxy path - a blocked scheduler thread stops draining its ring and wedges
// the processor - so the proxy park fails loud when this is set.
static thread_local bool schedulerThread = false;

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
        int r = ::munmap(stack, FiberScheduler::getOptions().fiberStackSize + 2 * getPageSize());
        SILK_ASSERT(!r);

        if (FiberScheduler::getOptions().accountMemoryUnmapped)
        {
            FiberScheduler::getOptions().accountMemoryUnmapped(
                static_cast<uint8_t *>(stack) + getPageSize(), FiberScheduler::getOptions().fiberStackSize);
        }
    }
}

bool Fiber::initialize(
    FiberId fiberId_, FiberMain * fiberMain_, FiberParametersDtor * parametersDtor_, FiberFuture * waitingFuture_) noexcept
{
    state.store(FiberState::SUSPENDED, std::memory_order_relaxed);

    inThreadMode = false;
    processorNumber = kInvalidProcessorNumber;
    suspendedProcessorNumber = kInvalidProcessorNumber;
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
        stack = ::mmap(nullptr, fiberStackSize + 2 * getPageSize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack == MAP_FAILED) [[unlikely]]
        {
            stack = nullptr;
            return false;
        }

        int r = ::mprotect(stack, getPageSize(), PROT_NONE);
        SILK_ASSERT(!r);

        r = ::mprotect(static_cast<uint8_t *>(stack) + getPageSize() + fiberStackSize, getPageSize(), PROT_NONE);
        SILK_ASSERT(!r);

        if (FiberScheduler::getOptions().accountMemoryMapped)
        {
            FiberScheduler::getOptions().accountMemoryMapped(static_cast<uint8_t *>(stack) + getPageSize(), fiberStackSize);
        }
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
        static_cast<uint8_t *>(stack) + getPageSize() + fiberStackSize, fiberStackSize, fiberContextMain);

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
        &schedulerFakeStack, static_cast<uint8_t *>(stack) + getPageSize(), FiberScheduler::getOptions().fiberStackSize);
#endif

#if defined(__SANITIZE_THREAD__)
    tsanSchedulerFiber = TSAN_FIBER_GET_CURRENT();
    TSAN_FIBER_SWITCH(tsanFiber);
#endif

    // Save the scheduler's C++ exception state and load this fiber's, so an exception being
    // propagated on either side is not clobbered by the other (they share the OS thread).
    CxaEhGlobals schedulerEh = loadExceptionState();
    storeExceptionState(cxaEhGlobals);

    auto transfer = boost::context::detail::jump_fcontext(fiberContext, this);
    // transfer is populated by the uninstrumented jump_fcontext assembly; MSan cannot see those
    // writes, so mark it initialized (as fiberContextMain does on first entry).
    MSAN_UNPOISON(&transfer, sizeof(transfer));
    fiberContext = transfer.fctx;

    // The scheduler resumed; restore its exception state.
    storeExceptionState(schedulerEh);

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

    // Save this fiber's C++ exception state; the scheduler restores it (via switchToFiberContext)
    // before this fiber next resumes.
    cxaEhGlobals = loadExceptionState();

    auto transfer = boost::context::detail::jump_fcontext(threadContext, nullptr);
    // transfer is populated by the uninstrumented jump_fcontext assembly; MSan cannot see those
    // writes, so mark it initialized (as fiberContextMain does on first entry).
    MSAN_UNPOISON(&transfer, sizeof(transfer));
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
    // Only external threads may block through the proxy. On a scheduler thread this
    // park means a completion callback or another thread-context hook issued a
    // blocking call inside the service loop - the processor would stop draining its
    // ring until someone else completes the wait.
    SILK_ASSERT(!schedulerThread, "a completion callback blocked the scheduler thread");

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

static void accountRingMemoryMappings(const io_uring & ring, MemoryMapCallback * callback) noexcept
{
    if (!callback)
    {
        return;
    }

    callback(ring.sq.sqes, ring.sq.sqes_sz);
    callback(ring.sq.ring_ptr, ring.sq.ring_sz);

    // The kernel serves both rings from one mapping given IORING_FEAT_SINGLE_MMAP.
    if (ring.cq.ring_ptr != ring.sq.ring_ptr)
    {
        callback(ring.cq.ring_ptr, ring.cq.ring_sz);
    }
}

void FiberScheduler::ProcessorState::initialize(uint16_t cpu) noexcept
{
    SILK_ASSERT(cpu < kInvalidProcessorNumber);
    number = cpu;

    readyQueue.initialize(options.readyQueueCapacity);

    eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    SILK_ASSERT(eventFd >= 0);

    io_uring_params params{};
    int r = ::io_uring_queue_init_params(options.ioUringQueueSize, &ring, &params);
    SILK_ASSERT(!r);

    // The kernel queues overflowing CQEs (failing submit with EBUSY) rather than dropping completions,
    // so a transiently full CQ never silently loses one - submitIo relies on it.
    SILK_ASSERT(params.features & IORING_FEAT_NODROP);

    // postWakeup posts cross-ring doorbells with IOSQE_CQE_SKIP_SUCCESS to drop the send-side completion.
    SILK_ASSERT(params.features & IORING_FEAT_CQE_SKIP);

    // Without IORING_FEAT_NO_IOWAIT, the kernel may account parked threads as iowait CPU usage.
    parkEnterFlags = IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG;
    if (params.features & IORING_FEAT_NO_IOWAIT)
    {
        parkEnterFlags |= IORING_ENTER_NO_IOWAIT;
    }

    accountRingMemoryMappings(ring, options.accountMemoryMapped);

    // Arm the wakeup doorbell. The kernel can end the multishot poll on CQ overflow,
    // so handleCompletionQueueSlow re-arms it through the same path on F_MORE loss.
    enqueueDoorbell();

    if (options.enableProfiler)
    {
        profiler = std::make_unique<Profiler>();
    }

    uint64_t nowCycles = Tsc::getCycles();
    lastServiceCycles.store(nowCycles, std::memory_order_relaxed);
    lastSubmitCycles.store(nowCycles, std::memory_order_relaxed);
    windowStartCycles = nowCycles;
}

void FiberScheduler::ProcessorState::destroy() noexcept
{
    if (eventFd >= 0)
    {
        accountRingMemoryMappings(ring, options.accountMemoryUnmapped);
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
    // seq_cst fence pairs with the one in parkThread to prevent the store-buffering race on weak
    // memory models (e.g. arm64): the producer's prior queue store and this sleeping load must not
    // reorder, so either we observe sleeping=true here (and post the eventfd) or parkThread's
    // hasWork() re-check observes our enqueued work. Same pattern as enqueueWaiter / releaseWaiters.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    if (sleeping.load(std::memory_order_acquire))
    {
        Perf::getSimpleCounter(simpleCounters[SCHEDULER_THREAD_WAKED], number).increment();

        int r = ::eventfd_write(eventFd, 1);
        SILK_ASSERT(!r);
    }
}

bool FiberScheduler::ProcessorState::parkThread(uint64_t waitNs, CpuTimer * timer) noexcept
{
    __kernel_timespec ts;
    ts.tv_sec = static_cast<int64_t>(waitNs / 1'000'000'000);
    ts.tv_nsec = static_cast<int64_t>(waitNs % 1'000'000'000);

    // waitNs of zero parks without a timeout.
    io_uring_getevents_arg arg{};
    arg.ts = waitNs ? reinterpret_cast<uint64_t>(&ts) : 0;

    Perf::getSimpleCounter(simpleCounters[SCHEDULER_THREAD_PARKED], number).increment();

    timer->reset(simpleCounters[SCHEDULER_IDLE_TIME], number);

    bool parkExpired = false;

    int r = ::io_uring_enter2(ring.ring_fd, 0, 1, parkEnterFlags, &arg, sizeof(arg));
    if (r < 0)
    {
        // io_uring_enter2 returns -errno directly; it does not set errno.
        // ETIME: timeout expired with no CQE (normal); EINTR: signal interrupted (normal);
        // EBUSY: a full CQ blocked the overflow flush (the caller's drain clears it).
        SILK_ASSERT(-r == ETIME || -r == EINTR || -r == EBUSY);
        if (-r == ETIME)
        {
            parkExpired = true;
        }
    }

    timer->reset(simpleCounters[SCHEDULER_SYSTEM_TIME], number);

    return parkExpired;
}

void FiberScheduler::ProcessorState::publishSleepDeadline() noexcept
{
    SleepFuture * nextSleep = sleepTree.min();
    sleepDeadlineCycles.store(nextSleep ? nextSleep->deadlineCycles : 0, std::memory_order_relaxed);
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
    if (cqReady())
    {
        return true;
    }
    return false;
}

// The kernel and other submitters update the SQ/CQ ring counters with plain
// stores; a racy read is benign - a stale count only defers or repeats work -
// so hide it from TSan here rather than annotating every call site.
uint32_t FiberScheduler::ProcessorState::sqReady() const noexcept
{
    TSAN_IGNORE_BEGIN();
    uint32_t count = ::io_uring_sq_ready(&ring);
    TSAN_IGNORE_END();
    return count;
}

uint32_t FiberScheduler::ProcessorState::cqReady() const noexcept
{
    TSAN_IGNORE_BEGIN();
    uint32_t count = ::io_uring_cq_ready(&ring);
    TSAN_IGNORE_END();
    return count;
}

void FiberScheduler::ProcessorState::enqueueDoorbell() noexcept
{
    // (Re-)arm the wakeup doorbell: a persistent multishot poll on eventFd posts a
    // CQE each time wakeThread writes to it. The kernel ends the poll on CQ overflow
    // (a CQE without IORING_CQE_F_MORE), so the completion drain re-arms it here. A
    // missed re-arm leaves the doorbell deaf forever, so retry through a full SQ ring.
    while (!enqueueIo(
        nullptr,
        [this](io_uring_sqe * sqe) noexcept
        {
            ::io_uring_prep_poll_multishot(sqe, eventFd, POLLIN);
            ::io_uring_sqe_set_data64(sqe, CQE_TAG_DOORBELL);
        }))
    {
        submitIo(true);
    }

    submitIo(true);
}

bool FiberScheduler::ProcessorState::postWakeup(ProcessorState * target) noexcept
{
    // Same lost-wakeup handshake as wakeThread: this seq_cst fence pairs with parkProcessor, so a target
    // observed awake here cannot have missed the ready-queue store its hasWork re-check sees.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // A running target dispatches its own queue; false tells the caller to check the backlog.
    if (!target->sleeping.load(std::memory_order_acquire))
    {
        return false;
    }

    Perf::getSimpleCounter(simpleCounters[SCHEDULER_THREAD_WAKED], number).increment();

    enqueueWakeup(target);
    submitIo(true);

    return true;
}

void FiberScheduler::ProcessorState::enqueueWakeup(ProcessorState * target) noexcept
{
    // Fill (do not submit) a doorbell SQE on this (the caller's) ring posting straight into the target's
    // CQ via IORING_OP_MSG_RING, waking its io_uring_enter2 - cheaper than the eventfd doorbell.
    // IOSQE_CQE_SKIP_SUCCESS drops our send-side completion, so only the target's CQE lands, tagged
    // CQE_TAG_WAKEUP for the drain to skip. A momentarily full SQ ring drains via submit and the fill
    // retries; the caller submits for delivery.
    int targetRingFd = target->ring.ring_fd;

    while (!enqueueIo(
        nullptr,
        [targetRingFd](io_uring_sqe * sqe) noexcept
        {
            ::io_uring_prep_msg_ring(sqe, targetRingFd, 0, CQE_TAG_WAKEUP, 0);
            ::io_uring_sqe_set_data64(sqe, CQE_TAG_WAKEUP);
            ::io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
        }))
    {
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

            // Record which processor holds this SQE so cancelIo can submit the
            // cancel to the correct ring (cross-ring cancels fail with -ENOENT).
            future->processorNumber = this->number;

            if (profiler)
            {
                future->submitTimestamp = Tsc::getCycles();
                future->category = getCurrentFiberId().category;
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
    // thresholds haven't been met. Kept small so it inlines into runFiber;
    // the rest lives in submitIoSlow.
    uint32_t count = sqReady();
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

    uint32_t count = sqReady();
    if (count == 0)
    {
        return false;
    }

    // TSan needs an explicit barrier between submission/completion.
    TSAN_RELEASE(this);

    int r = ::io_uring_submit(&ring);

    // Under IORING_FEAT_NODROP the kernel returns EBUSY (EINTR/EAGAIN are likewise
    // transient) when the CQ ring is full of unreaped completions: it refuses new
    // SQEs whose completions it could not store. The SQEs stay queued, so defer
    // rather than abort - the service loop drains the CQ, which schedules fibers and
    // re-submits via handleReadyQueue. lastSubmitCycles is left stale on deferral so
    // the staleness gate in submitIo retries promptly.
    if (r < 0)
    {
        SILK_ASSERT(r == -EBUSY || r == -EINTR || r == -EAGAIN);
        return false;
    }

    lastSubmitCycles.store(startCycles, std::memory_order_relaxed);

    if (profiler)
    {
        profileEvent(ProfileEventKind::SUBMIT_IO, 0, Tsc::getCycles() - startCycles);
    }

    // io_uring_submit reports how many SQEs it actually consumed, which can be fewer
    // than were ready on a partial submit; count the real number, not sq_ready.
    Perf::getSimpleCounter(simpleCounters[IO_ENQUEUED], number).increment(static_cast<uint64_t>(r));
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

FiberScheduler::SchedulerState::SchedulerState() noexcept
{
    int r = ::sem_init(&threadSemaphore, 0, 0);
    SILK_ASSERT(!r);
}

FiberScheduler::SchedulerState::~SchedulerState() noexcept
{
    // The suspended lists link nodes embedded in pool-owned fibers, so the
    // processors must go before fiberPool frees the fiber memory.
    processorState.reset();

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

cpu_set_t FiberScheduler::defaultCpuMask() noexcept
{
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        CPU_SET(cpu, &cpuSet);
    }

    return cpuSet;
}

void FiberScheduler::initialize(const Options * userOptions) noexcept
{
    SILK_ASSERT(!scheduler);

    REGISTER_SIMPLE_COUNTERS(&simpleCounters, FIBER_SIMPLE_COUNTERS);

    if (userOptions)
    {
        options = *userOptions;
    }

    SILK_ASSERT(options.fiberStackSize >= getPageSize() && (options.fiberStackSize % getPageSize()) == 0);
    SILK_ASSERT(options.readyQueueCapacity >= 2 && (options.readyQueueCapacity & (options.readyQueueCapacity - 1)) == 0);
    SILK_ASSERT(options.readyDispatchBatch >= 1);
    SILK_ASSERT(options.ioUringQueueSize >= 2 && (options.ioUringQueueSize & (options.ioUringQueueSize - 1)) == 0);
    SILK_ASSERT(options.ioUringFlushThreshold >= 1 && options.ioUringFlushThreshold <= options.ioUringQueueSize);
    SILK_ASSERT(options.waiterTableSize >= 2 && (options.waiterTableSize & (options.waiterTableSize - 1)) == 0);

    options.ioUringFlushTimeoutCycles = Tsc::nanosecondsToCycles(options.ioUringFlushTimeout);
    options.backlogAgeCycles = Tsc::nanosecondsToCycles(options.maxWaitNs);
    options.spinThresholdCycles = Tsc::nanosecondsToCycles(options.spinThresholdNs);

    scheduler = new SchedulerState();
    scheduler->cpuController.initialize(options.backlogAgeCycles);

    scheduler->waiterTable = std::make_unique<WaitStack[]>(options.waiterTableSize);
    scheduler->waiterTableMask = options.waiterTableSize - 1;

    scheduler->processorCount = getProcessorCount();
    SILK_ASSERT(
        scheduler->processorCount <= kInvalidProcessorNumber,
        "configured CPU count %d exceeds the supported maximum %d",
        scheduler->processorCount,
        kInvalidProcessorNumber);
    scheduler->processorState = std::make_unique<ProcessorState[]>(scheduler->processorCount);

    cpu_set_t processCpuSet;
    CPU_ZERO(&processCpuSet);
    int r = ::sched_getaffinity(0, sizeof(processCpuSet), &processCpuSet);
    if (r)
    {
        r = errno;
        SILK_FAIL("could not read the process affinity mask: r=%d", r);
    }

    CPU_ZERO(&scheduler->activeMask);
    scheduler->homeProcessor = std::make_unique<ProcessorState *[]>(scheduler->processorCount);

    uint16_t activeCount = 0;
    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        if (isCpuActive(cpu, processCpuSet, options.cpuMask))
        {
            scheduler->processorState[cpu].number = cpu;
            CPU_SET(cpu, &scheduler->activeMask);
            ++activeCount;
        }
    }

    SILK_ASSERT(activeCount > 0, "cpuMask excludes all affinity-mask cpus");

    // Route every CPU to an active home: an active CPU to itself, an inactive
    // one to an active CPU taken round-robin so injection from a reserved core
    // spreads across the active rings instead of piling onto one.
    uint16_t nextHome = 0;
    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        if (scheduler->processorState[cpu].number != kInvalidProcessorNumber)
        {
            scheduler->homeProcessor[cpu] = &scheduler->processorState[cpu];
            continue;
        }

        while (scheduler->processorState[nextHome].number == kInvalidProcessorNumber)
        {
            nextHome = (nextHome + 1) % scheduler->processorCount;
        }
        scheduler->homeProcessor[cpu] = &scheduler->processorState[nextHome];
        nextHome = (nextHome + 1) % scheduler->processorCount;
    }

    scheduler->schedulerThreadCount = activeCount;
    scheduler->schedulerThreads = std::make_unique<std::thread[]>(scheduler->schedulerThreadCount);

    buildStealCandidates();

    // From here on the active set is read from activeMask, never from
    // ProcessorState::number: a started scheduler thread writes its own number in
    // ProcessorState::initialize, so reading it from this thread would race.
    uint16_t threadIndex = 0;
    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        if (CPU_ISSET(cpu, &scheduler->activeMask))
        {
            scheduler->schedulerThreads[threadIndex++] = std::thread(runScheduler, &scheduler->processorState[cpu]);
        }
    }

    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        if (CPU_ISSET(cpu, &scheduler->activeMask))
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

    for (uint16_t i = 0; i < scheduler->workerThreadCount; ++i)
    {
        scheduler->workerThreads[i] = std::thread(runThreadWorker);
    }
}

void FiberScheduler::buildStealCandidates() noexcept
{
    auto topologies = std::make_unique<CpuTopology[]>(scheduler->processorCount);
    readCpuTopologies(topologies.get(), scheduler->processorCount);

    uint16_t candidateCount = scheduler->processorCount - 1;
    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        if (processor->number == kInvalidProcessorNumber)
        {
            continue;
        }

        // Build an array of CPUs with the estimated stealing cost.
        processor->stealCandidates = std::make_unique<StealCandidate[]>(candidateCount);

        uint16_t i = 0;
        for (uint16_t other = 0; other < scheduler->processorCount; ++other)
        {
            if (other == cpu)
            {
                continue;
            }
            uint64_t cost = UINT64_MAX;
            if (scheduler->processorState[other].number != kInvalidProcessorNumber)
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
        for (uint16_t start = 0; start < candidateCount;)
        {
            uint64_t groupCost = processor->stealCandidates[start].costCycles;
            uint16_t end = start;
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

    // Record each active CPU's active HT sibling for the prefix order.
    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        if (processor->number == kInvalidProcessorNumber || topologies[cpu].coreId == UINT32_MAX)
        {
            continue;
        }

        for (uint16_t other = 0; other < scheduler->processorCount; ++other)
        {
            bool sameCore = topologies[other].packageId == topologies[cpu].packageId && topologies[other].coreId == topologies[cpu].coreId;
            if (other != cpu && sameCore && scheduler->processorState[other].number != kInvalidProcessorNumber)
            {
                processor->siblingProcessor = other;
                break;
            }
        }
    }

    // Build the prefix order - whole cores first, HT siblings after - so growth always
    // engages a whole idle core before any sibling of a running CPU. The scheduler boots
    // at full width and the idle decay shrinks the prefix from the right.
    scheduler->prefixOrder = std::make_unique<uint16_t[]>(scheduler->processorCount);
    uint16_t orderIndex = 0;

    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        if (processor->number != kInvalidProcessorNumber
            && (processor->siblingProcessor == kInvalidProcessorNumber || cpu < processor->siblingProcessor))
        {
            processor->prefixIndex = orderIndex;
            scheduler->prefixOrder[orderIndex++] = cpu;
        }
    }

    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        if (processor->number != kInvalidProcessorNumber && processor->siblingProcessor != kInvalidProcessorNumber
            && cpu > processor->siblingProcessor)
        {
            processor->prefixIndex = orderIndex;
            scheduler->prefixOrder[orderIndex++] = cpu;
        }
    }

    scheduler->prefixTotal = orderIndex;
    scheduler->firstProcessor = &scheduler->processorState[scheduler->prefixOrder[0]];
    scheduler->prefixCount.store(orderIndex, std::memory_order_relaxed);
}

void FiberScheduler::destroy() noexcept
{
    SILK_ASSERT(scheduler);

    scheduler->stopping.store(true, std::memory_order_release);

    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        if (processor->number != kInvalidProcessorNumber)
        {
            processor->wakeThread();
        }
    }

    for (uint16_t i = 0; i < scheduler->schedulerThreadCount; ++i)
    {
        scheduler->schedulerThreads[i].join();
    }

    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        processor->destroy();
    }

    for (uint16_t i = 0; i < scheduler->workerThreadCount; ++i)
    {
        scheduler->wakeThread();
    }

    for (uint16_t i = 0; i < scheduler->workerThreadCount; ++i)
    {
        scheduler->workerThreads[i].join();
    }

    // A fiber still linked here suspended (or stayed scheduled) and never ran
    // to completion: the caller leaked it, violating the contract that no
    // fibers exist at destroy time. Fail here, where the leak is attributable,
    // instead of corrupting teardown.
    SILK_ASSERT(scheduler->readyQueue.empty(), "fiber leaked: still in the global ready queue");
    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        SILK_ASSERT(processor->suspendedList.empty(), "fiber leaked: still suspended on cpu %u", cpu);

        // readyQueue.empty() touches the slot array, which exists only for
        // processors that were actually initialized.
        if (processor->number != kInvalidProcessorNumber)
        {
            SILK_ASSERT(processor->readyQueue.empty(), "fiber leaked: still ready on cpu %u", cpu);
        }
    }

    delete scheduler;
}

// noinline is load-bearing, not a hint. These accessors read the threadFiber/proxyFiber
// thread-locals. A fiber may suspend on one OS thread and resume on another, so a caller
// that brackets a suspension point must observe the resuming thread's value.
// If the accessor were inlined, the compiler could materialize the thread pointer once
// into a callee-saved register and reuse it after the suspension; jump_fcontext preserves
// callee-saved registers across the switch, so the cached pointer would still address the
// previous (now idle) thread's thread-local, reading a stale value or null.
// Keeping the access behind a real call boundary forces a fresh thread-pointer read on
// every invocation.
__attribute__((noinline)) FiberId FiberScheduler::getCurrentFiberId() noexcept
{
    return threadFiber ? threadFiber->fiberId : FiberId{};
}

__attribute__((noinline)) Fiber * FiberScheduler::getCurrentFiber() noexcept
{
    // Fast path: thread is running a regular fiber, or has already lazily
    // allocated a proxy fiber.
    if (threadFiber)
    {
        return threadFiber;
    }
    if (!proxyFiber) [[unlikely]]
    {
        // Lazily create a proxy fiber so a non-fiber thread can still participate
        // in fiber-aware APIs (e.g. FiberMutex::lock, FiberScheduler::run-and-wait).
        proxyFiber = std::make_unique<Fiber>(true);
    }
    return proxyFiber.get();
}

bool FiberScheduler::isFiberRunning(Fiber * fiber) noexcept
{
    return fiber->state.load(std::memory_order_acquire) == FiberState::RUNNING;
}

// The processor a caller on the current CPU injects work into: its own processor
// if the CPU runs a scheduler thread, otherwise the active home mapped in
// initialize. This keeps injection from a reserved core off the uninitialized
// ring of an inactive CPU (which would index out of bounds at
// kInvalidProcessorNumber). Re-reads the current CPU fresh on every call - never
// cache it across a suspension.
FiberScheduler::ProcessorState * FiberScheduler::currentProcessor() noexcept
{
    return scheduler->homeProcessor[getCurrentProcessor()];
}

Fiber *
FiberScheduler::allocateFiber(FiberMain * fiberMain, FiberParametersDtor * parametersDtor, uint8_t category, FiberFuture * future) noexcept
{
    Fiber * fiber = scheduler->fiberPool.allocate();
    if (fiber)
    {
        ProcessorState * processor = currentProcessor();
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
        ProcessorState * processor = currentProcessor();
        ProcessorState * target = enqueueReady(processor, fiber);
        if (target && !processor->postWakeup(target))
        {
            // Running target: the fiber sits in its queue and the running fiber may
            // never yield, so check the backlog stamp.
            growPrefix(processor, target);
        }
        return true;
    }

    return false;
}

// Shared by schedule, scheduleAll, and runFiber: place a ready fiber on its home ready queue and return
// the processor whose doorbell the caller must ring (immediately, or batched). processor is the caller's
// injection processor (the current CPU's own processor, or its active home when the current CPU is outside
// the active set) - used as the home default and, by the caller, as the doorbell source - so it must come
// from currentProcessor. A proxy or thread-mode fiber, or a full ready queue, rings its own wakeup here and
// returns null.
FiberScheduler::ProcessorState * FiberScheduler::enqueueReady(ProcessorState * processor, Fiber * fiber) noexcept
{
    if (!fiber->isProxyFiber)
    {
        if (options.enableProfiler)
        {
            fiber->submitTimestamp = Tsc::getCycles();
        }

        if (!fiber->inThreadMode)
        {
            uint16_t prefixCount = scheduler->prefixCount.load(std::memory_order_relaxed);
            ProcessorState * target = nullptr;

            if (fiber->processorNumber != kInvalidProcessorNumber)
            {
#if !defined(NDEBUG) || defined(__SANITIZE_THREAD__)
                if (scheduler->schedulerThreadCount > 1 && prefixCount > 1)
                {
                    uint16_t shuffleIndex = scheduler->processorState[fiber->processorNumber].prefixIndex;
                    fiber->processorNumber = scheduler->prefixOrder[(shuffleIndex + 1) % prefixCount];
                }
#endif
                target = &scheduler->processorState[fiber->processorNumber];

                bool targetAwake = target->prefixIndex < prefixCount && !target->sleeping.load(std::memory_order_relaxed);
                bool producerAwake = processor->prefixIndex < prefixCount && !processor->sleeping.load(std::memory_order_relaxed);
                bool producerQueueEmpty = processor->readyQueue.empty();

                // The home keeps the fiber while it can run it right away. A home outside
                // the prefix must not hold work and always migrates; a parked home
                // migrates below full width when an awake prefix producer with an empty
                // ready queue can run the fiber with its data still warm - a producer
                // holding queued work stops attracting, else wakes recentralize on the
                // loaded member and undo every steal. At full width there is no capacity
                // to shed and the parked home keeps its fiber.
                if (!targetAwake
                    && (target->prefixIndex >= prefixCount
                        || (producerAwake && producerQueueEmpty && prefixCount != scheduler->prefixTotal)))
                {
                    target = nullptr;
                }
            }

            // A fiber without a usable home - none assigned yet, or dropped above -
            // migrates: the producer takes it while it is a prefix member, with the
            // wake's data warm in its cache; the first processor otherwise.
            if (!target)
            {
                target = processor->prefixIndex < prefixCount ? processor : scheduler->firstProcessor;
                fiber->processorNumber = target->number;
            }

            if (target->readyQueue.enqueue(fiber))
            {
                Perf::getSimpleCounter(simpleCounters[FIBER_ENQUEUED], processor->number).increment();
                return target;
            }

            // Ready queue full: fall back to the worker thread pool.
            Perf::getSimpleCounter(simpleCounters[READY_QUEUE_FULL], processor->number).increment();
        }

        scheduler->readyQueue.enqueue(fiber);
        Perf::getSimpleCounter(simpleCounters[FIBER_ENQUEUED_SHARED], processor->number).increment();
        scheduler->wakeThread();
    }
    else
    {
        fiber->wakeThread();
    }

    return nullptr;
}

void FiberScheduler::scheduleAll(Fiber ** fibers, uint64_t count) noexcept
{
    ProcessorState * processor = currentProcessor();

    // Dedup the wake targets in a bitmap over all processors so each parked target is rung exactly once.
    uint64_t words[Bitmap::wordCount(kInvalidProcessorNumber)];
    Bitmap wakeTargets(words, scheduler->processorCount);
    wakeTargets.clear();

    for (uint64_t i = 0; i < count; i++)
    {
        Fiber * fiber = fibers[i];
        if (!fiber->tryChangeStateToReady())
        {
            continue;
        }

        ProcessorState * target = enqueueReady(processor, fiber);
        if (target)
        {
            wakeTargets.set(target->number);
        }
    }

    // One StoreLoad barrier for the whole batch (same handshake as postWakeup): the ready-queue stores
    // above are ordered before the sleeping loads below, so a target parking concurrently is not missed.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Ring each distinct parked target once, filling one doorbell SQE per target; a single submit delivers
    // them all (and drains the SQ for the retry if it momentarily fills).
    bool enqueued = false;
    for (uint32_t bit = 0; wakeTargets.findBit(bit, true, &bit); bit++)
    {
        ProcessorState * target = &scheduler->processorState[bit];
        if (!target->sleeping.load(std::memory_order_acquire))
        {
            // Running target: its queued fibers wait, so check the backlog stamp -
            // the target may be inside a fiber that never yields.
            growPrefix(processor, target);
            continue;
        }

        Perf::getSimpleCounter(simpleCounters[SCHEDULER_THREAD_WAKED], processor->number).increment();

        processor->enqueueWakeup(target);
        enqueued = true;
    }

    if (enqueued)
    {
        processor->submitIo(true);
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

void FiberScheduler::suspend(SuspendCallback * callback, void * context, uint64_t * waitCycles) noexcept
{
    uint64_t suspendStart = waitCycles ? Tsc::getCycles() : 0;

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

    if (waitCycles)
    {
        *waitCycles += Tsc::getCycles() - suspendStart;
    }
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
        processor = currentProcessor();
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

void FiberScheduler::read(int fd, iovec * iov, uint32_t iov_len, uint64_t offset, uint64_t * bytesRead, IoFuture * future) noexcept
{
    future->result = bytesRead;
#if defined(__SANITIZE_MEMORY__)
    future->readIov = iov;
    future->readIovLen = iov_len;
#endif
    enqueueIo(future, [=](io_uring_sqe * sqe) noexcept { ::io_uring_prep_readv(sqe, fd, iov, iov_len, offset); });
}

void FiberScheduler::write(int fd, iovec * iov, uint32_t iov_len, uint64_t offset, uint64_t * bytesWritten, IoFuture * future) noexcept
{
    future->result = bytesWritten;
    enqueueIo(future, [=](io_uring_sqe * sqe) noexcept { ::io_uring_prep_writev(sqe, fd, iov, iov_len, offset); });
}

void FiberScheduler::readFixed(
    int fd, void * buf, uint32_t len, uint64_t offset, int bufIndex, uint64_t * bytesRead, IoFuture * future) noexcept
{
    future->result = bytesRead;
#if defined(__SANITIZE_MEMORY__)
    future->readIovStorage = {buf, len};
    future->readIov = &future->readIovStorage;
    future->readIovLen = 1;
#endif
    enqueueIo(future, [=](io_uring_sqe * sqe) noexcept { ::io_uring_prep_read_fixed(sqe, fd, buf, len, offset, bufIndex); });
}

void FiberScheduler::writeFixed(
    int fd, const void * buf, uint32_t len, uint64_t offset, int bufIndex, uint64_t * bytesWritten, IoFuture * future) noexcept
{
    future->result = bytesWritten;
    enqueueIo(future, [=](io_uring_sqe * sqe) noexcept { ::io_uring_prep_write_fixed(sqe, fd, buf, len, offset, bufIndex); });
}

void FiberScheduler::registerBuffers(const iovec * iovecs, unsigned count) noexcept
{
    SILK_ASSERT(scheduler, "registerBuffers called before initialize()");
    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
    {
        ProcessorState * processor = &scheduler->processorState[cpu];
        if (processor->number == kInvalidProcessorNumber)
        {
            continue;
        }
        int r = ::io_uring_register_buffers(&processor->ring, iovecs, count);
        SILK_ASSERT(r == 0, "io_uring_register_buffers failed: r=%d, cpu=%u", r, cpu);
    }
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

void FiberScheduler::splice(
    int fdIn,
    int64_t offsetIn,
    int fdOut,
    int64_t offsetOut,
    uint64_t len,
    uint32_t flags,
    uint64_t * bytesSpliced,
    IoFuture * future) noexcept
{
    future->result = bytesSpliced;
    uint32_t boundedLen = static_cast<uint32_t>(std::min<uint64_t>(len, UINT32_MAX));
    enqueueIo(
        future, [=](io_uring_sqe * sqe) noexcept { ::io_uring_prep_splice(sqe, fdIn, offsetIn, fdOut, offsetOut, boundedLen, flags); });
}

void FiberScheduler::cancelIo(IoFuture * future) noexcept
{
    // The cancel SQE must go to the SAME io_uring ring that holds the original
    // SQE.  If we submit the cancel to a different ring (e.g. because the fiber
    // was work-stolen to another CPU between registering the poll and cancelling
    // it), io_uring returns -ENOENT and the original operation is never removed,
    // leaving the caller's IoFuture::wait() blocked forever.
    uint16_t processorNumber = future->processorNumber;
    if (processorNumber == kInvalidProcessorNumber)
    {
        processorNumber = currentProcessor()->number;
    }

    ProcessorState * processor = &scheduler->processorState[processorNumber];

    auto setup = [=](io_uring_sqe * sqe) noexcept
    {
        ::io_uring_prep_cancel(sqe, future, 0);
        ::io_uring_sqe_set_data64(sqe, CQE_TAG_CANCEL);
    };

    // Retry if the SQ ring is temporarily full.
    while (!processor->enqueueIo(nullptr, setup))
    {
        Perf::getSimpleCounter(simpleCounters[SQ_RING_OVERFLOW], processor->number).increment();
        yield();
    }

    // If we enqueued to a remote processor's ring, force-submit.
    if (processorNumber != getCurrentProcessor() || getCurrentFiber()->isProxyFiber)
    {
        processor->submitIo(true);
    }
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
    future->processorNumber = currentProcessor()->number;

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
    for (uint16_t cpu = 0; cpu < scheduler->processorCount; ++cpu)
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
    schedulerThread = true;

    int r = pinThreadToCpu(processor->number);
    SILK_ASSERT(!r, "could not pin the scheduler thread to its cpu: r=%d", r);

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

        uint64_t nowCycles = Tsc::getCycles();
        if (didWork)
        {
            idleSinceCycles = nowCycles;
            waitNs = 0;
        }
        else
        {
            waitNs = waitNs ? std::min<uint64_t>(waitNs * 2, options.maxWaitNs) : options.initialWaitNs;
        }

        // Evaluate the width window on every pass, busy or idle - adjustPrefix itself
        // detects the window boundary and returns cheaply inside one.
        adjustPrefix(processor, nowCycles);
    }
}

bool FiberScheduler::runServiceLoop(ProcessorState * processor, uint64_t waitNs, CpuTimer * timer) noexcept
{
    // Only one thread runs the service loop for a given processor at a time.
    if (!processor->serviceLoopLock.try_lock())
    {
        return false;
    }

    bool parkExpired = false;

    if (waitNs)
    {
        // Cap the park at the earliest sleep deadline; cross-proc wakeups still ring the doorbell.
        uint64_t effectiveWaitNs = waitNs;
        SleepFuture * nextSleep = processor->sleepTree.min();
        if (nextSleep)
        {
            uint64_t nowCycles = Tsc::getCycles();
            if (nowCycles >= nextSleep->deadlineCycles)
            {
                effectiveWaitNs = 0;
            }
            else
            {
                uint64_t untilNs = Tsc::cyclesToNanoseconds(nextSleep->deadlineCycles - nowCycles);
                effectiveWaitNs = std::min(effectiveWaitNs, untilNs);
            }
        }

        // Wait step starts at initialWaitNs and doubles each idle iteration up to maxWaitNs;
        // past the indefinite-park threshold parkThread stretches the timeout to the rare
        // backstop, so a fully idle scheduler barely polls - wakeups are doorbell-driven.
        // Before going to sleep - spin a little to avoid eventfd syscalls.
        if (effectiveWaitNs)
        {
            if (effectiveWaitNs < options.spinThresholdNs)
            {
                bool spinHit = spinWait([=] { return processor->hasWork() || scheduler->stopping.load(std::memory_order_relaxed); });
                processor->window.countWait(spinHit);
            }
            else
            {
                bool deadlineBounded = nextSleep != nullptr;
                parkExpired = parkProcessor(processor, effectiveWaitNs, deadlineBounded, timer);
            }
        }
    }

    // Drain all pending work that arrived while we were waiting (or immediately, when waitNs=0).
    bool didWork = false;
    didWork |= handleCompletionQueue(processor);
    didWork |= handleSleepQueue(processor);
    didWork |= handleCancelQueue(processor);
    didWork |= handleExpiredWaiters(processor);

    // An expired park whose drain finds due work (a ripe sleeper) is demand; an empty expiry is waste.
    if (parkExpired)
    {
        processor->window.countWait(didWork);
    }

    // Aggregate profile events.
    if (processor->profiler)
    {
        processor->profiler->aggregate();
    }

    // The attendance heartbeat: stale means this processor's queues went unchecked.
    processor->lastServiceCycles.store(Tsc::getCycles(), std::memory_order_relaxed);

    processor->serviceLoopLock.unlock();
    return didWork;
}

bool FiberScheduler::runStealLoop(ProcessorState * processor, uint64_t idleSinceCycles, CpuTimer * timer) noexcept
{
    bool didWork = false;

    // Budget for stealing equals idle time: don't steal from a CPU whose topology
    // cost exceeds how long we have been idle, and don't spend more time stealing
    // than we have already sat idle. A freshly-idle processor skips stealing
    // entirely and lets the backoff in runServiceLoop accumulate idle time first.
    uint64_t nowCycles = Tsc::getCycles();
    uint64_t idleCycles = nowCycles - idleSinceCycles;
    uint64_t deadlineCycles = nowCycles + idleCycles;

    uint16_t candidateCount = scheduler->processorCount - 1;
    for (uint16_t i = 0; i < candidateCount && nowCycles < deadlineCycles; ++i)
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

        // A victim attended within the spin horizon runs its own work sooner than a steal can move it.
        if (victim->lastServiceCycles.load(std::memory_order_relaxed) + options.spinThresholdCycles > nowCycles)
        {
            continue;
        }

        didWork |= runServiceLoop(victim, 0, timer);

        // We have a limited budget to spend doing work for others.
        bool stoleAny = false;
        for (nowCycles = Tsc::getCycles(); nowCycles < deadlineCycles; nowCycles = Tsc::getCycles())
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

            // A stolen run is productive work - without it a pure thief would read as
            // pure idle and take the one-window shrink shortcut.
            processor->window.countDispatched(1);
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

bool FiberScheduler::parkProcessor(ProcessorState * processor, uint64_t waitNs, bool deadlineBounded, CpuTimer * timer) noexcept
{
    // Flush deferred SQEs: the idle path has no other submit, and parking
    // passes to_submit=0, so a deferred doorbell rearm, MSG_RING wakeup, or
    // remote cancel would sit queued until unrelated activity lands on this
    // ring. If the submit defers again: on EBUSY hasWork sees the full CQ
    // and skips the park; on EAGAIN the timed park is the retry backoff.
    processor->submitIo(true);

    // Announce that we are about to park, then a seq_cst fence pairing with the one in wakeThread:
    // release alone is not a StoreLoad barrier, so without this the store could reorder past the
    // re-checks below while a concurrent wakeThread reads sleeping=false - both miss, and
    // the wakeup is lost on weak memory models (e.g. arm64). Same pattern as enqueueWaiter.
    // The same fence publishes the prefix shrink for the producers' backlog checks.
    processor->sleeping.store(true, std::memory_order_release);

    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Classify the role behind the fence: either this read sees the new width, or the
    // grower's fence-paired sleeping check sees the park and rings. Outside the prefix
    // the park goes indefinite; the first processor right of it is the timed poller.
    bool indefinitePark = false;
    bool standby = false;
    if (waitNs >= options.maxWaitNs && !processor->sqReady())
    {
        uint16_t prefixCount = scheduler->prefixCount.load(std::memory_order_relaxed);
        if (!deadlineBounded && processor->prefixIndex > prefixCount)
        {
            indefinitePark = true;
        }
        if (processor->prefixIndex == prefixCount)
        {
            standby = true;
        }
    }

    // Double-check: work may have arrived between the last drain and here.
    // If so, skip the park entirely so that work is not delayed by waitNs.
    bool parking = !processor->hasWork();

    if (parking && (indefinitePark || standby))
    {
        // Pre-park sweep behind the fence - a hit aborts the park and the steal loop
        // takes it. A standby hit self-activates; the next poller was appointed first.
        if (sweepBacklog(processor))
        {
            if (standby)
            {
                startProcessor(processor, processor->prefixIndex, Tsc::getCycles());
            }

            parking = false;
        }
    }

    bool parkExpired = false;

    if (parking)
    {
        // Outside the prefix the park has no timeout - wakeups are doorbell-driven.
        if (indefinitePark)
        {
            waitNs = 0;
        }

        parkExpired = processor->parkThread(waitNs, timer);

        // A park cut short by arriving work is a rewarded wait; an expiry is classified after the drain.
        if (!parkExpired)
        {
            processor->window.countWait(true);
        }
    }

    processor->sleeping.store(false, std::memory_order_relaxed);

    return parkExpired;
}

bool FiberScheduler::sweepBacklog(ProcessorState * processor) noexcept
{
    if (options.disableWorkStealing)
    {
        return false;
    }

    // Cheapest-first sweep for aged backlog and unattended work. A hit restarts the
    // stamp's age - the sweeper is about to take the backlog. The aged-backlog signal
    // passes the same grow gate as every door - a standby re-activating into a spread
    // that never pays, or past a pending probe, is the same untracked growth; the
    // unattended rescue stays unconditional.
    uint64_t nowCycles = Tsc::getCycles();
    uint16_t prefixCount = scheduler->prefixCount.load(std::memory_order_relaxed);
    bool growApproved = scheduler->cpuController.approveGrow(prefixCount, scheduler->prefixTotal, nowCycles);

    uint16_t candidateCount = scheduler->processorCount - 1;
    for (uint16_t i = 0; i < candidateCount; ++i)
    {
        StealCandidate * candidate = &processor->stealCandidates[i];
        if (candidate->costCycles == UINT64_MAX)
        {
            break;
        }

        ProcessorState * neighbor = &scheduler->processorState[candidate->processorNumber];
        if (!neighbor->initialized.load(std::memory_order_acquire))
        {
            continue;
        }

        // The observation also arms backlog enqueued at full width - producers only
        // stamp below it; a hit restarts the age, the sweeper is about to take the
        // backlog.
        if (growApproved && !neighbor->readyQueue.empty())
        {
            if (scheduler->cpuController.observeBacklog(&neighbor->backlogSinceCycles, nowCycles))
            {
                neighbor->backlogSinceCycles.store(nowCycles, std::memory_order_relaxed);
                return true;
            }
        }

        // Work present but unattended: ring completions or a due sleep deadline behind
        // a stale heartbeat. A parked owner self-wakes; an attending one never hits.
        if (!neighbor->sleeping.load(std::memory_order_relaxed))
        {
            uint64_t lastServiceCycles = neighbor->lastServiceCycles.load(std::memory_order_relaxed);
            if (nowCycles - lastServiceCycles >= options.backlogAgeCycles)
            {
                if (neighbor->cqReady())
                {
                    return true;
                }

                uint64_t sleepDeadline = neighbor->sleepDeadlineCycles.load(std::memory_order_relaxed);
                if (sleepDeadline != 0 && nowCycles >= sleepDeadline)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

bool FiberScheduler::startProcessor(ProcessorState * producer, uint16_t prefixCount, uint64_t nowCycles) noexcept
{
    // Claim the one growth per window - every door passes here, so the pace holds and
    // the growth stamp is monotonic; a claim burned by the width CAS losing its race
    // costs one window of growth.
    if (!scheduler->cpuController.claimGrow(nowCycles))
    {
        return false;
    }

    // Appoint the next poller before the width moves - the timed observer never
    // lapses; a lost race costs one spurious doorbell.
    if (prefixCount + 1 < scheduler->prefixTotal)
    {
        producer->postWakeup(&scheduler->processorState[scheduler->prefixOrder[prefixCount + 1]]);
    }

    // Widen the prefix from the expected width by one; false when the width moved.
    if (!scheduler->prefixCount.compare_exchange_weak(prefixCount, prefixCount + 1, std::memory_order_relaxed))
    {
        return false;
    }


    Perf::getSimpleCounter(simpleCounters[SCHEDULER_THREAD_GROW], producer->number).increment();

    // Ring the started processor - its steal loop finds and re-homes the work; a
    // self-activating standby is already awake and needs no doorbell.
    ProcessorState * started = &scheduler->processorState[scheduler->prefixOrder[prefixCount]];

    if (started != producer)
    {
        Perf::getSimpleCounter(simpleCounters[SCHEDULER_THREAD_WAKED], producer->number).increment();

        producer->enqueueWakeup(started);
        producer->submitIo(true);
    }

    return true;
}

void FiberScheduler::adjustPrefix(ProcessorState * processor, uint64_t nowCycles) noexcept
{
    if (options.disableWorkStealing || options.disableCpuAdjust)
    {
        return;
    }

    if (nowCycles - processor->windowStartCycles < options.backlogAgeCycles)
    {
        return;
    }

    uint16_t prefixCount = scheduler->prefixCount.load(std::memory_order_relaxed);
    if (processor->prefixIndex >= prefixCount)
    {
        return;
    }

    adjustPrefixSlow(processor, nowCycles, prefixCount);
}

__attribute__((noinline)) void
FiberScheduler::adjustPrefixSlow(ProcessorState * processor, uint64_t nowCycles, uint16_t prefixCount) noexcept
{
    uint64_t elapsedCycles = nowCycles - processor->windowStartCycles;
    CpuController::Decision decision = scheduler->cpuController.evaluateWindow(
        &processor->window, processor->prefixIndex, prefixCount, scheduler->prefixTotal, elapsedCycles, nowCycles);
    processor->windowStartCycles = nowCycles;

    if (decision.action == CpuController::Action::GROW)
    {
        startProcessor(processor, prefixCount, nowCycles);
        return;
    }

    if (decision.action == CpuController::Action::SHRINK)
    {
        if (scheduler->prefixCount.compare_exchange_weak(prefixCount, decision.width, std::memory_order_relaxed))
        {
            Perf::getSimpleCounter(simpleCounters[SCHEDULER_THREAD_SHRINK], processor->number).increment();
            scheduler->cpuController.commitShrink(&processor->window);
        }
    }
}

void FiberScheduler::growPrefix(ProcessorState * producer, ProcessorState * target) noexcept
{
    if (options.disableWorkStealing || options.disableCpuAdjust)
    {
        return;
    }

    uint16_t prefixCount = scheduler->prefixCount.load(std::memory_order_relaxed);
    if (prefixCount == scheduler->prefixTotal)
    {
        return;
    }

    growPrefixSlow(producer, target, prefixCount);
}

__attribute__((noinline)) void
FiberScheduler::growPrefixSlow(ProcessorState * producer, ProcessorState * target, uint16_t prefixCount) noexcept
{
    // The first observation arms the stamp; only backlog older than a full window
    // grows - a stalled owner's queue stays non-empty through its stalls, where a
    // closed wake loop touches empty and disarms. Growth restarts the age, pacing
    // one processor per target per window.
    uint64_t nowCycles = Tsc::getCycles();

    if (!scheduler->cpuController.observeBacklog(&target->backlogSinceCycles, nowCycles))
    {
        return;
    }

    if (!scheduler->cpuController.approveGrow(prefixCount, scheduler->prefixTotal, nowCycles))
    {
        return;
    }

    if (startProcessor(producer, prefixCount, nowCycles))
    {
        target->backlogSinceCycles.store(nowCycles, std::memory_order_relaxed);
    }
}

bool FiberScheduler::handleReadyQueue(ProcessorState * processor, CpuTimer * timer) noexcept
{
    uint32_t dispatched = 0;

    // Bound the batch so runServiceLoop runs every pass: an unbounded drain lets
    // a self-re-enqueuing yield loop starve timer expiry and io_uring completions.
    for (uint32_t i = 0; i < options.readyDispatchBatch; ++i)
    {
        Fiber * fiber;
        if (processor->readyQueue.dequeue(&fiber))
        {
            runFiber(fiber, timer);
            ++dispatched;
        }
        else
        {
            // Drained empty - disarm the backlog stamp.
            if (processor->backlogSinceCycles.load(std::memory_order_relaxed) != 0)
            {
                processor->backlogSinceCycles.store(0, std::memory_order_relaxed);
            }

            break;
        }
    }

    processor->window.countDispatched(dispatched);

    // Drain whatever the dispatch left below the pressure-relief threshold
    // so the kernel sees it before the scheduler thread parks.
    if (dispatched)
    {
        processor->submitIo(true);
    }

    return dispatched != 0;
}

bool FiberScheduler::handleCompletionQueue(ProcessorState * processor) noexcept
{
    // Fast path: CQ ring is empty, nothing to drain.
    uint32_t count = processor->cqReady();
    if (count == 0)
    {
        return false;
    }

    return handleCompletionQueueSlow(processor);
}

__attribute__((noinline)) bool FiberScheduler::handleCompletionQueueSlow(ProcessorState * processor) noexcept
{
    bool didWork = false;
    bool rearmDoorbell = false;

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
                // IO cancellation confirmation
                continue;
            }

            if (tag == CQE_TAG_DOORBELL)
            {
                // Wakeup doorbell: drain the eventfd counter. The kernel ends the
                // multishot poll on CQ overflow - a CQE without IORING_CQE_F_MORE -
                // so re-arm it after the drain or wakeThread can no longer wake us.
                eventfd_t val;
                ::eventfd_read(processor->eventFd, &val);

                if (!(cqe->flags & IORING_CQE_F_MORE))
                {
                    rearmDoorbell = true;
                }
                continue;
            }

            if (tag == CQE_TAG_WAKEUP)
            {
                // Cross-ring wakeup doorbell (IORING_OP_MSG_RING): a pure wakeup carrier - the work is
                // already in the ready queue. Covers both an incoming doorbell and the sender-side
                // completion of an outgoing one. Nothing to drain or re-arm.
                continue;
            }

            // IO completion. Every IO op is one-shot - only the doorbell is multishot -
            // so each IoFuture completes exactly once. A multishot IO op added later
            // (recv/accept multishot) would deliver IORING_CQE_F_MORE here and set the
            // same future repeatedly; trip loudly rather than double-complete silently.
            SILK_ASSERT(!(cqe->flags & IORING_CQE_F_MORE));

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
#if defined(__SANITIZE_MEMORY__)
                // MSan cannot see the kernel filling read buffers via io_uring.
                uint64_t remaining = static_cast<uint64_t>(result);
                for (uint32_t i = 0; i < future->readIovLen && remaining > 0; ++i)
                {
                    const uint64_t n = std::min<uint64_t>(remaining, future->readIov[i].iov_len);
                    MSAN_UNPOISON(future->readIov[i].iov_base, n);
                    remaining -= n;
                }
#endif
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

    if (rearmDoorbell)
    {
        processor->enqueueDoorbell();
    }

    if (didWork && processor->profiler)
    {
        uint64_t durationCycles = entryCycles - processor->lastServiceCycles.load(std::memory_order_relaxed);
        processor->profileEvent(ProfileEventKind::CQ_WAIT, 0, durationCycles);
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

    processor->publishSleepDeadline();
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

    processor->publishSleepDeadline();

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

    uint64_t nowCycles = Tsc::getCycles();
    if (sleepFuture->deadlineCycles > nowCycles)
    {
        return false;
    }

    handleExpiredWaitersSlow(processor, sleepFuture, nowCycles);
    return true;
}

__attribute__((noinline)) void
FiberScheduler::handleExpiredWaitersSlow(ProcessorState * processor, SleepFuture * sleepFuture, uint64_t nowCycles) noexcept
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

    } while (sleepFuture && sleepFuture->deadlineCycles <= nowCycles);

    processor->publishSleepDeadline();

    Perf::getSimpleCounter(simpleCounters[SLEEP_EXPIRED], processor->number).increment(count);
}

void FiberScheduler::runFiber(Fiber * fiber, CpuTimer * timer) noexcept
{
    fiber->changeState(FiberState::READY, FiberState::RUNNING);

    // Maintain the per-CPU suspended list for GDB debuggability.
    // suspendedLock and suspendedList are co-located in ProcessorState cache line 0.
    // Benchmarking showed no net cost.
    if (fiber->suspendedProcessorNumber != kInvalidProcessorNumber)
    {
        ProcessorState * processor = &scheduler->processorState[fiber->suspendedProcessorNumber];
        processor->removeSuspended(fiber);
        fiber->suspendedProcessorNumber = kInvalidProcessorNumber;
    }

    ProcessorState * processor = &scheduler->processorState[fiber->processorNumber];

    // Only the per-CPU scheduler thread (timer != nullptr) reports profile events:
    // it is pinned and is the sole producer of this CPU's SPSC ring. Worker threads
    // (timer == nullptr) migrate within the active set and would break the
    // single-producer rule.
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

    // Fiber gains the CPU: fires on the first run and on every resume. Runs on
    // the scheduler thread immediately before control transfers into the fiber;
    // since jump_fcontext is a same-thread stack switch, anything the callback
    // installs (e.g. thread-local execution context) is visible to fiber code.
    if (options.fiberResume)
    {
        options.fiberResume(fiber);
    }

    fiber->switchToFiberContext();

    // Fiber relinquishes the CPU: fires whether it suspended (will resume later)
    // or stopped (terminated), so fiberResume/fiberSuspend calls always balance.
    if (options.fiberSuspend)
    {
        options.fiberSuspend(fiber);
    }

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
    // amortize the syscall across multiple fibers; on worker threads there is
    // no batching boundary, so force-submit per fiber.
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

            ProcessorState * target = enqueueReady(processor, fiber);
            if (target && !processor->postWakeup(target))
            {
                // Running target: the fiber sits in its queue and the running fiber may
                // never yield, so check the backlog stamp.
                growPrefix(processor, target);
            }
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
    // Confine the overflow pool to the active CPUs so silk never runs a fiber on
    // a reserved core, and so a worker never injects from an inactive CPU.
    int r = pinThreadToCpus(scheduler->activeMask);
    SILK_ASSERT(!r, "could not pin the worker thread to the active cpu set: r=%d", r);

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
