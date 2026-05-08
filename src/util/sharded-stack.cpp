#include <silk/util/sharded-stack.h>

#include <silk/util/assert.h>
#include <silk/util/sanitizers.h>

#include <new>

// Suppress warnings emitted by librseq headers: volatile assignment in rseq_cs
// and unused parameters in the asm stubs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-volatile"
#pragma clang diagnostic ignored "-Wunused-parameter"
#include <rseq/rseq.h>
#pragma clang diagnostic pop

namespace silk
{

ShardedStackBase::ShardedStackBase(uint32_t batchSize) noexcept
    : batchSize(batchSize)
    , processorCount(getProcessorCount())
    , processorState(std::make_unique<ProcessorState[]>(processorCount))
{
    for (uint32_t i = 0; i < processorCount; ++i)
    {
        FreeList * freeList = acquireFreeList();
        SILK_ASSERT(freeList);
        emptyFreeLists.push(freeList);
    }
}

ShardedStackBase::~ShardedStackBase() noexcept
{
    while (FreeList * freeList = emptyFreeLists.pop())
    {
        releaseFreeList(freeList);
    }

    while (FreeList * freeList = fullFreeLists.pop())
    {
        releaseFreeList(freeList);
    }
}

void ShardedStackBase::push(StackEntry * entry) noexcept
{
    for (;;)
    {
        uint32_t cpu = rseq_cpu_start();
        ProcessorState * state = &processorState[cpu];

        uint32_t count = state->count.load(std::memory_order_relaxed);
        if (count < batchSize)
        {
            // Prepare the link before the critical section.
            // Release ordering publishes all writes to entry's content (e.g. deinitialisation)
            // before the entry becomes visible to a concurrent pop.
            // The matching acquire is in pop after the rseq commit.
            StackEntry * oldHead = state->head.load(std::memory_order_relaxed);
            entry->next.store(oldHead, std::memory_order_release);

            // Fast path: rseq critical section, zero atomic instructions.
            //
            //   load head; if head != oldHead -> ne (head changed, retry);
            //   commit: head = entry.
            //
            // The ne check catches the case where we migrated to a new CPU between
            // reading oldHead and the commit -- the new CPU's head is unlikely to
            // equal oldHead, and even if it does the rseq abort (ret == -1) fires
            // on migration before the commit can execute.
            TSAN_IGNORE_BEGIN();
            int ret = rseq_load_cbne_store__ptr(
                RSEQ_MO_RELAXED, // only supported memory order
                RSEQ_PERCPU_CPU_ID, // identify CPU via cpu_id field
                state->headPtr(), // per-CPU head pointer
                reinterpret_cast<intptr_t>(oldHead), // return 1 if head changed
                reinterpret_cast<intptr_t>(entry), // store entry as new head on success
                static_cast<int>(cpu)); // expected CPU ID
            TSAN_IGNORE_END();

            if (ret == 0)
            {
                // count is a heuristic; a stale increment triggers the slow path
                // one operation early at worst. count was captured before the rseq
                // so no RMW needed -- plain store avoids the LOCK prefix.
                state->count.store(count + 1, std::memory_order_relaxed);
                return;
            }

            // ret == 1 (head changed) or -1 (preempted): retry.
        }
        else
        {
            // Slow path: per-CPU list full, move it to the global pool.
            flush(state);
        }
    }
}

void ShardedStackBase::flush(ProcessorState * state) noexcept
{
    // acq_rel: acquire observes any in-progress rseq commit;
    // release publishes the null so subsequent rseq reads see an empty list.
    StackEntry * head = state->head.exchange(nullptr, std::memory_order_acq_rel);
    state->count.store(0, std::memory_order_relaxed);

    if (head)
    {
        StackEntry * tail = head;
        while (StackEntry * next = tail->next.load(std::memory_order_relaxed))
        {
            tail = next;
        }
        pushBatch(head, tail);
    }
}

StackEntry * ShardedStackBase::pop() noexcept
{
    for (;;)
    {
        uint32_t cpu = rseq_cpu_start();
        ProcessorState * state = &processorState[cpu];

        uint32_t count = state->count.load(std::memory_order_relaxed);

        // Fast path: rseq critical section, zero atomic instructions.
        //
        //   load head; if null -> eq (slow path);
        //   save head to entry; load head->next (voffp=0); commit: head = next.
        //
        // If preempted before the commit the kernel restarts from the top of
        // the loop (ret == -1). The commit is a plain store; no CAS needed.
        intptr_t entry = 0;
        TSAN_IGNORE_BEGIN();
        int ret = rseq_load_cbeq_store_add_load_store__ptr(
            RSEQ_MO_RELAXED, // only supported memory order
            RSEQ_PERCPU_CPU_ID, // identify CPU via cpu_id field
            state->headPtr(), // per-CPU head pointer
            0, // return 1 if head == NULL (empty list)
            0, // byte offset from head to next pointer; StackEntry::next is at offset 0
            &entry, // receives the old head value (the popped entry)
            static_cast<int>(cpu)); // expected CPU ID
        TSAN_IGNORE_END();

        if (ret == 0)
        {
            // Acquire synchronizes with the release store of entry->next in push,
            // establishing happens-before: all writes before push are visible here.
            reinterpret_cast<StackEntry *>(entry)->next.load(std::memory_order_acquire);

            // count is a heuristic; a stale decrement triggers the slow path
            // one operation early at worst. count was captured before the rseq
            // so no RMW needed -- plain store avoids the LOCK prefix.
            state->count.store(count - 1, std::memory_order_relaxed);
            return reinterpret_cast<StackEntry *>(entry);
        }

        if (ret == 1)
        {
            // Slow path: per-CPU list empty, pull a batch from the global pool.
            if (!refill(state))
            {
                return nullptr;
            }
            continue;
        }

        // ret == -1 (preempted): retry.
    }
}

bool ShardedStackBase::refill(ProcessorState * state) noexcept
{
    // Slow path for pop: pull a batch from the global pool and install it as the
    // per-CPU list. Returns true if at least one entry is now available.
    FreeList * freeList = fullFreeLists.pop();
    if (!freeList) [[unlikely]]
    {
        return false;
    }

    StackEntry * newHead = freeList->entries.popAll();
    emptyFreeLists.push(freeList);

    if (!newHead)
    {
        return false;
    }

    // Walk the chain to find the tail and actual count. Both are needed below:
    // the tail for pushBatch on CAS failure, the count to correctly initialize
    // ProcessorState::count (which may be less than batchSize if the FreeList
    // was partially filled by an explicit flush rather than a full overflow).
    StackEntry * tail = newHead;
    uint32_t actualCount = 1;
    while (StackEntry * next = tail->next.load(std::memory_order_relaxed))
    {
        tail = next;
        actualCount++;
    }

    // Install the batch as the per-CPU list. Use CAS rather than a plain store:
    // if we migrated between entering the slow path and reaching this point,
    // another thread may have pushed to this CPU's head concurrently, and a
    // plain store would clobber that entry.
    StackEntry * expected = state->head.load(std::memory_order_relaxed);
    for (;;)
    {
        if (expected)
        {
            pushBatch(newHead, tail);
            break;
        }

        if (state->head.compare_exchange_weak(expected, newHead, std::memory_order_relaxed, std::memory_order_relaxed))
        {
            state->count.store(actualCount, std::memory_order_relaxed);
            break;
        }
    }

    return true;
}

void ShardedStackBase::pushBatch(StackEntry * head, StackEntry * tail) noexcept
{
    FreeList * freeList = emptyFreeLists.pop();
    if (!freeList) [[unlikely]]
    {
        freeList = acquireFreeList();
        SILK_ASSERT(freeList);
    }
    freeList->entries.pushAll(head, tail);
    fullFreeLists.push(freeList);
}

void ShardedStackBase::flush() noexcept
{
    for (uint32_t i = 0; i < processorCount; ++i)
    {
        flush(&processorState[i]);
    }
}

void ShardedStackBase::drain(DrainCallback * callback, void * ctx) noexcept
{
    flush();

    while (FreeList * freeList = fullFreeLists.pop())
    {
        StackEntry * entry = freeList->entries.popAll();
        while (entry)
        {
            StackEntry * next = entry->next.load(std::memory_order_relaxed);
            callback(entry, ctx);
            entry = next;
        }
        emptyFreeLists.push(freeList);
    }
}

ShardedStackBase::FreeList * ShardedStackBase::acquireFreeList() noexcept
{
    // Ignore non-atomic initialization
    TSAN_IGNORE_BEGIN();
    FreeList * freeList = new (std::nothrow) FreeList;
    TSAN_IGNORE_END();
    return freeList;
}

void ShardedStackBase::releaseFreeList(FreeList * freeList) noexcept
{
    delete freeList;
}

} // namespace silk
