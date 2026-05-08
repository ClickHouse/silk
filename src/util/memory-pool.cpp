#include <silk/util/memory-pool.h>

#include <silk/util/assert.h>
#include <silk/util/sanitizers.h>

#include <new>

namespace silk
{

MemoryPoolBase::MemoryPoolBase(
    uint32_t objectSize, uint32_t alignment, uint32_t stackEntryOffset, InitFn * initialize, DestroyFn * destroy) noexcept
    : objectSize(alignUp(objectSize, alignment))
    , alignment(alignment)
    , chunkSize(alignUp<uint32_t>(slotsOffset() + MIN_BATCH_SIZE * this->objectSize, PAGE_SIZE))
    , stackEntryOffset(stackEntryOffset)
    , initialize(initialize)
    , destroy(destroy)
    , stack((chunkSize - slotsOffset()) / this->objectSize)
{
    uint32_t processorCount = getProcessorCount();
    for (uint32_t i = 0; i < processorCount; ++i)
    {
        Chunk * chunk = allocateChunk();
        SILK_ASSERT(chunk);
    }
}

MemoryPoolBase::~MemoryPoolBase() noexcept
{
    if (destroy)
    {
        stack.drain([](StackEntry * entry, void * ctx) noexcept { static_cast<MemoryPoolBase *>(ctx)->destroy(entry); }, this);
    }

    while (Chunk * chunk = chunks.pop())
    {
        freeChunk(chunk);
    }
}

StackEntry * MemoryPoolBase::allocate() noexcept
{
    for (;;)
    {
        StackEntry * entry = stack.pop();
        if (entry)
        {
            return entry;
        }

        Chunk * chunk = allocateChunk();
        if (!chunk) [[unlikely]]
        {
            // TODO(vskipin): use ShardedStack::flush
            return nullptr;
        }
    }
}

MemoryPoolBase::Chunk * MemoryPoolBase::allocateChunk() noexcept
{
    // Ignore non-atomic initialization
    TSAN_IGNORE_BEGIN();
    void * mem = ::operator new(chunkSize, std::align_val_t{alignment}, std::nothrow);
    TSAN_IGNORE_END();

    if (!mem) [[unlikely]]
    {
        return nullptr;
    }

    Chunk * chunk = new (mem) Chunk;
    StackEntry * head = nullptr;
    StackEntry * tail = nullptr;

    uint32_t offset = slotsOffset();
    uint32_t freelistSize = (chunkSize - offset) / objectSize;

    uint8_t * slots = reinterpret_cast<uint8_t *>(chunk) + offset;
    for (uint32_t i = 0; i < freelistSize; ++i)
    {
        StackEntry * entry = reinterpret_cast<StackEntry *>(slots + i * objectSize + stackEntryOffset);
        if (initialize)
        {
            // Ignore non-atomic initialization
            TSAN_IGNORE_BEGIN();
            initialize(entry);
            TSAN_IGNORE_END();
        }

        entry->next.store(head, std::memory_order_relaxed);
        if (!tail)
        {
            tail = entry;
        }
        head = entry;
    }

    stack.pushBatch(head, tail);
    chunks.push(chunk);
    return chunk;
}

void MemoryPoolBase::freeChunk(Chunk * chunk) noexcept
{
    std::destroy_at(chunk);
    ::operator delete(chunk, std::align_val_t{alignment});
}

} // namespace silk
