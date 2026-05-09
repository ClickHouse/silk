#pragma once

#include <silk/util/platform.h>
#include <silk/util/sharded-stack.h>

#include <cstdint>
#include <memory>
#include <type_traits>

namespace silk
{

/**
 * Lock-free, type-erased memory pool.
 *
 * Memory is allocated in chunks. Each chunk is carved into fixed-size slots.
 * Freed objects are returned to a per-CPU sharded free list and reused before
 * new chunks are allocated. Objects are constructed on first allocation from a
 * raw chunk and destroyed when the pool itself is destroyed.
 *
 * Use the typed wrapper MemoryPool instead of this class directly.
 */
class MemoryPoolBase
{
public:
    using InitFn = void(StackEntry *);
    using DestroyFn = void(StackEntry *);

    /** Create a pool for objects of the given size, alignment, and StackEntry offset within each object. */
    MemoryPoolBase(uint32_t objectSize, uint32_t alignment, uint32_t stackEntryOffset, InitFn * initialize, DestroyFn * destroy) noexcept;

    /** Destroy all allocated chunks, calling destroy on every object that was ever constructed. */
    ~MemoryPoolBase() noexcept;

    /** Allocate an object from the free list, growing the pool if necessary. */
    [[nodiscard]] StackEntry * allocate() noexcept;

    /** Return an object to the free list. */
    void deallocate(StackEntry * entry) noexcept { stack.push(entry); }

private:
    //
    // Constants.
    //

    static constexpr uint32_t MIN_BATCH_SIZE = 16;

    /**
     * Chunk header for a block sized to hold exactly freelistSize object slots.
     * Object slots follow at the first alignment-multiple offset past the header;
     * each slot is objectSize bytes wide and holds one constructed object.
     */
    struct Chunk
    {
        StackEntry stackEntry;
    };

    //
    // Helpers.
    //

    uint32_t slotsOffset() const noexcept { return alignUp<uint32_t>(sizeof(Chunk), alignment); }
    Chunk * allocateChunk() noexcept;
    void freeChunk(Chunk * chunk) noexcept;

    //
    // State.
    //

    uint32_t objectSize;
    uint32_t alignment;
    uint32_t chunkSize;
    uint32_t stackEntryOffset;
    InitFn * initialize;
    DestroyFn * destroy;
    ShardedStackBase stack;
    LockFreeStack<Chunk, &Chunk::stackEntry> chunks;
};

/**
 * Typed lock-free memory pool.
 *
 * T must embed a StackEntry member pointed to by @p nodePtr. Calls T's
 * constructor/destructor via std::construct_at and std::destroy_at.
 */
template <typename T, StackEntry T::* nodePtr>
class MemoryPool
{
public:
    /** Create a pool for objects of type T. */
    MemoryPool() noexcept
        : impl(
              sizeof(T),
              alignof(T),
              static_cast<uint32_t>(memberOffset(nodePtr)),
              std::is_trivially_constructible_v<T> ? nullptr : initializeObject,
              std::is_trivially_destructible_v<T> ? nullptr : destroyObject)
    {
    }

    /** Allocate an object from the pool, growing it if necessary. */
    [[nodiscard]] T * allocate() noexcept
    {
        StackEntry * entry = impl.allocate();
        return entry ? entryToObject(entry) : nullptr;
    }

    /** Return an object to the pool. */
    void deallocate(T * object) noexcept { impl.deallocate(objectToEntry(object)); }

private:
    //
    // Helpers.
    //

    static T * entryToObject(StackEntry * entry) noexcept { return containerOf(entry, nodePtr); }
    static StackEntry * objectToEntry(T * object) noexcept { return &(object->*nodePtr); }

    static void initializeObject(StackEntry * entry) { std::construct_at(entryToObject(entry)); }
    static void destroyObject(StackEntry * entry) { std::destroy_at(entryToObject(entry)); }

    //
    // State.
    //

    MemoryPoolBase impl;
};

} // namespace silk
