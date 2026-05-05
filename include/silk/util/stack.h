#pragma once

#include <silk/util/platform.h>

#include <atomic>
#include <cstdint>
#include <utility>

namespace silk
{

/**
 * Intrusive stack node. Objects stored in a Stack or LockFreeStack must embed this.
 */
struct StackEntry
{
    std::atomic<StackEntry *> next;
};

/**
 * Lock-free LIFO stack using 128-bit tagged pointers to prevent the ABA problem.
 * Requires 128-bit lock-free atomics: CMPXCHG16B on x86-64, CASP (LSE) on aarch64.
 *
 * Objects are intrusive: each element must embed a StackEntry, and the caller
 * passes raw StackEntry pointers. Use the typed wrapper LockFreeStack instead.
 */
class LockFreeStackBase
{
public:
    /** Push an entry onto the top of the stack. */
    void push(StackEntry * entry) noexcept { pushAll(entry, entry); }

    /**
     * Atomically splice a pre-formed chain [head -> ... -> tail -> ?] onto the
     * top of the stack.  On return, tail->next points to what was previously the
     * top.  The caller must own every node in [head..tail] and must have already
     * set up the intra-chain next pointers before calling this.
     */
    void pushAll(StackEntry * head, StackEntry * tail) noexcept
    {
        TaggedPtr currentHead = this->head.load(std::memory_order_relaxed);
        for (;;)
        {
            tail->next.store(currentHead.ptr, std::memory_order_relaxed);

            TaggedPtr newHead{head, currentHead.tag + 1};

            // Success: release - ensures all prior writes to the pushed entries
            // (including tail->next and any caller writes to entry content) are
            // visible to any thread that acquires the new top via pop/popAll.
            //
            // Failure: relaxed - we only use currentHead.ptr as the new tail->next
            // value (a pointer copy), never dereferencing through it, so no
            // synchronization with the entry's publisher is required.
            if (this->head.compare_exchange_weak(currentHead, newHead, std::memory_order_release, std::memory_order_relaxed))
            {
                return;
            }
        }
    }

    /** Pop and return the top entry, or nullptr if the stack is empty. */
    StackEntry * pop() noexcept
    {
        TaggedPtr currentHead = head.load(std::memory_order_acquire);
        for (;;)
        {
            if (!currentHead.ptr)
            {
                return nullptr;
            }

            TaggedPtr newHead{currentHead.ptr->next.load(std::memory_order_relaxed), currentHead.tag + 1};

            // Success: acq_rel - acquire synchronizes with the push that published
            // this entry, making its content visible to the caller. Release is not
            // strictly required (pop publishes nothing) but is conventional for RMW.
            //
            // Failure: acquire - currentHead is immediately dereferenced on the next
            // iteration (currentHead.ptr->next). Acquire ensures the entry's next
            // pointer, written before its publisher's release push, is visible.
            if (head.compare_exchange_weak(currentHead, newHead, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                return currentHead.ptr;
            }
        }
    }

    /** Atomically detach and return all entries as a linked list, or nullptr if empty. */
    StackEntry * popAll() noexcept
    {
        TaggedPtr currentHead = head.load(std::memory_order_acquire);
        for (;;)
        {
            if (!currentHead.ptr)
            {
                return nullptr;
            }

            TaggedPtr newHead{nullptr, currentHead.tag + 1};

            // Success: acq_rel - acquire synchronizes with the push that published
            // the chain, making all entries visible to the caller for traversal.
            // Release covers the store of nullptr to top (no data published, but
            // acq_rel is semantically correct for an RMW that reads and writes).
            //
            // Failure: acquire - currentHead may be dereferenced on the next
            // iteration if the chain is non-empty; acquire ensures visibility of
            // the entry published by whoever last modified top.
            if (head.compare_exchange_weak(currentHead, newHead, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                return currentHead.ptr;
            }
        }
    }

    /** Return true if the stack is empty. */
    bool empty() const noexcept { return head.load(std::memory_order_relaxed).ptr == nullptr; }

private:
    /**
     * 128-bit tagged pointer; the tag prevents the ABA problem.
     */
    struct alignas(16) TaggedPtr
    {
        StackEntry * ptr;
        uint64_t tag;
    };

    static_assert(sizeof(TaggedPtr) == 16);
    static_assert(std::atomic<TaggedPtr>::is_always_lock_free);

    //
    // State.
    //

    std::atomic<TaggedPtr> head{TaggedPtr{}};
};

/**
 * Typed lock-free LIFO stack.
 *
 * T must embed a StackEntry member pointed to by @p nodePtr.
 */
template <typename T, StackEntry T::* nodePtr>
class LockFreeStack
{
public:
    /** Push an object onto the top of the stack. */
    void push(T * object) noexcept { impl.push(objectToEntry(object)); }

    /** Atomically splice a pre-formed chain [head -> ... -> tail] onto the stack. */
    void pushAll(T * head, T * tail) noexcept { impl.pushAll(objectToEntry(head), objectToEntry(tail)); }

    /** Pop and return the top object, or nullptr if the stack is empty. */
    T * pop() noexcept
    {
        StackEntry * entry = impl.pop();
        return entry ? entryToObject(entry) : nullptr;
    }

    /** Atomically detach and return all objects as a linked list, or nullptr if empty. */
    T * popAll() noexcept
    {
        StackEntry * entry = impl.popAll();
        return entry ? entryToObject(entry) : nullptr;
    }

    /** Return true if the stack is empty. */
    bool empty() const noexcept { return impl.empty(); }

    /** Return the next object in a list returned by popAll(), or nullptr at the end. */
    static T * next(T * object) noexcept
    {
        StackEntry * entry = objectToEntry(object)->next.load(std::memory_order_relaxed);
        return entry ? entryToObject(entry) : nullptr;
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

    LockFreeStackBase impl;
};

/**
 * Single-threaded LIFO stack operating on raw StackEntry pointers.
 * Not thread-safe; all operations must be externally synchronized.
 * Use the typed wrapper Stack instead.
 */
class StackBase
{
public:
    /** Push an entry onto the top of the stack. */
    void push(StackEntry * entry) noexcept
    {
        entry->next.store(head, std::memory_order_relaxed);
        head = entry;
    }

    /** Pop and return the top entry, or nullptr if the stack is empty. */
    StackEntry * pop() noexcept
    {
        StackEntry * entry = head;
        if (entry)
        {
            head = entry->next.load(std::memory_order_relaxed);
        }
        return entry;
    }

    /** Detach and return all entries as a linked list, or nullptr if empty. */
    StackEntry * popAll() noexcept { return std::exchange(head, nullptr); }

    /** Return true if the stack is empty. */
    bool empty() const noexcept { return head == nullptr; }

private:
    //
    // State.
    //

    StackEntry * head = nullptr;
};

/**
 * Typed single-threaded LIFO stack.
 *
 * T must embed a StackEntry member pointed to by @p nodePtr.
 * Not thread-safe; all operations must be externally synchronized.
 */
template <typename T, StackEntry T::* nodePtr>
class Stack
{
public:
    /** Push an object onto the top of the stack. */
    void push(T * object) noexcept { impl.push(objectToEntry(object)); }

    /** Pop and return the top object, or nullptr if the stack is empty. */
    T * pop() noexcept
    {
        StackEntry * entry = impl.pop();
        return entry ? entryToObject(entry) : nullptr;
    }

    /** Detach and return all objects as a linked list, or nullptr if empty. */
    T * popAll() noexcept
    {
        StackEntry * entry = impl.popAll();
        return entry ? entryToObject(entry) : nullptr;
    }

    /** Return true if the stack is empty. */
    bool empty() const noexcept { return impl.empty(); }

    /** Return the next object in a list returned by popAll(), or nullptr at the end. */
    static T * next(T * object) noexcept
    {
        StackEntry * entry = objectToEntry(object)->next.load(std::memory_order_relaxed);
        return entry ? entryToObject(entry) : nullptr;
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

    StackBase impl;
};

} // namespace silk
