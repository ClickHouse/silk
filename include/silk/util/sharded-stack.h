#pragma once

#include <silk/util/platform.h>
#include <silk/util/stack.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace silk
{

/**
 * Per-CPU stack backed by restartable sequences (rseq).
 *
 * The fast path (push/pop when the per-CPU list is non-empty/non-full) executes
 * zero atomic instructions. rseq guarantees atomicity with respect to preemption:
 * if the thread migrates or is preempted within the critical section the kernel
 * restarts it, so plain loads/stores suffice for per-CPU state.
 *
 * Each CPU holds a single linked list. When the list empties on pop, or fills on
 * push, a single 128-bit CAS transfers a batch to/from the global
 * fullFreeLists/emptyFreeLists pool, amortising its cost over batchSize operations.
 *
 * Objects are intrusive: each element must embed a StackEntry. Use the typed
 * wrapper ShardedStack instead of this class directly.
 */
class ShardedStackBase
{
public:
    explicit ShardedStackBase(uint32_t batchSize) noexcept;
    ~ShardedStackBase() noexcept;

    /** Push an entry onto the stack. */
    void push(StackEntry * entry) noexcept;

    /** Pop and return the top entry, or nullptr if the stack is empty. */
    StackEntry * pop() noexcept;

    /**
     * Push a pre-formed chain [head -> ... -> tail -> null] directly into the
     * global pool as a single full batch, bypassing per-CPU buffers.
     * Use when loading a batch of entries at once (e.g. a newly allocated chunk).
     */
    void pushBatch(StackEntry * head, StackEntry * tail) noexcept;

    /**
     * Move all per-CPU buffered entries to the global pool.
     * Call after all push/pop operations have completed -- a concurrent push/pop
     * will race with flush on that entry.
     */
    void flush() noexcept;

    /** Called by drain for each remaining entry. */
    using DrainCallback = void(StackEntry * entry, void * ctx) noexcept;

    /**
     * Flush per-CPU state, then invoke callback on every remaining entry.
     * Useful for cleanup on shutdown (e.g. calling destructors, releasing resources).
     */
    void drain(DrainCallback * callback, void * ctx) noexcept;

private:
    /**
     * A lock-free stack of objects.
     */
    struct FreeList
    {
        StackEntry stackEntry;
        LockFreeStackBase entries;
    };

    /**
     * Per-CPU state, padded to a cache line to prevent false sharing.
     * head is the per-CPU linked list; entries are pushed and popped via
     * rseq critical sections that require no atomic instructions.
     *
     * count is a heuristic: loaded and stored with relaxed ordering so a
     * stale value causes an unnecessary slow-path call at worst, never lost
     * objects or corruption. rseq guarantees the head update is atomic with
     * respect to preemption; count is updated after the rseq commit.
     */
    struct alignas(CACHELINE_SIZE) ProcessorState
    {
        std::atomic<StackEntry *> head{};
        std::atomic<uint32_t> count{};

        // Return a pointer to the raw StackEntry* stored inside head.
        // Used by rseq primitives that operate on intptr_t* (plain memory),
        // bypassing the C++ atomic interface.
        intptr_t * headPtr() noexcept { return reinterpret_cast<intptr_t *>(&head); }
    };

    //
    // Helpers.
    //

    void flush(ProcessorState * state) noexcept;
    bool refill(ProcessorState * state) noexcept;
    FreeList * acquireFreeList() noexcept;
    void releaseFreeList(FreeList * freeList) noexcept;

    //
    // State.
    //

    const uint32_t batchSize;
    const uint32_t processorCount;
    std::unique_ptr<ProcessorState[]> processorState;
    LockFreeStack<FreeList, &FreeList::stackEntry> emptyFreeLists;
    LockFreeStack<FreeList, &FreeList::stackEntry> fullFreeLists;
};

/**
 * Typed sharded stack.
 *
 * T must embed a StackEntry member pointed to by @p nodePtr.
 */
template <typename T, StackEntry T::* nodePtr>
class ShardedStack
{
public:
    explicit ShardedStack(uint32_t batchSize) noexcept
        : impl(batchSize)
    {
    }

    /** Push an object onto the stack. */
    void push(T * object) noexcept { impl.push(objectToEntry(object)); }

    /**
     * Push a pre-formed chain [head -> ... -> tail -> null] directly into the
     * global pool as a single full batch, bypassing per-CPU buffers.
     * Use when loading a batch of objects at once (e.g. during initialization).
     */
    void pushBatch(T * head, T * tail) noexcept { impl.pushBatch(objectToEntry(head), objectToEntry(tail)); }

    /** Pop and return the top object, or nullptr if the stack is empty. */
    T * pop() noexcept
    {
        StackEntry * entry = impl.pop();
        return entry ? entryToObject(entry) : nullptr;
    }

    /** Move all per-CPU buffered objects to the global pool. */
    void flush() noexcept { impl.flush(); }

    /** Called by drain for each remaining object. */
    using DrainCallback = void(T * object, void * ctx) noexcept;

    /** Flush per-CPU state, then invoke callback on every remaining object. */
    void drain(DrainCallback * callback, void * ctx) noexcept
    {
        struct Adapter
        {
            DrainCallback * callback;
            void * ctx;
        };
        Adapter adapter{callback, ctx};
        impl.drain(
            [](StackEntry * entry, void * adapterCtx) noexcept
            {
                auto * adapter = static_cast<Adapter *>(adapterCtx);
                adapter->callback(entryToObject(entry), adapter->ctx);
            },
            &adapter);
    }

private:
    //
    // Helpers.
    //

    static StackEntry * objectToEntry(T * object) noexcept { return &(object->*nodePtr); }
    static T * entryToObject(StackEntry * entry) noexcept { return containerOf(entry, nodePtr); }

    //
    // State.
    //

    ShardedStackBase impl;
};

} // namespace silk
