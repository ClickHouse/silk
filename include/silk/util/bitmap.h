#pragma once

#include <silk/util/platform.h>

#include <cstdint>
#include <cstring>

namespace silk
{

/**
 * Non-owning view over a bitmap of bitCount bits packed into 64-bit words. The words are caller-owned;
 * size that buffer with wordCount.
 */
class Bitmap
{
public:
    Bitmap(uint64_t * words, uint32_t bitCount) noexcept
        : words(words)
        , bitCount(bitCount)
    {
    }

    /** Number of 64-bit words a bitmap of bitCount bits occupies. */
    static constexpr uint32_t wordCount(uint32_t bitCount) noexcept { return alignUp(bitCount, WORD_BITS) / WORD_BITS; }

    /** True if the bit at index is set. */
    bool test(uint32_t index) const noexcept { return (words[index / WORD_BITS] >> (index % WORD_BITS)) & 1; }

    /** Set the bit at index. */
    void set(uint32_t index) noexcept { words[index / WORD_BITS] |= ONE << (index % WORD_BITS); }

    /** Clear the bit at index. */
    void clear(uint32_t index) noexcept { words[index / WORD_BITS] &= ~(ONE << (index % WORD_BITS)); }

    /** Zero every word. */
    void clear() noexcept { std::memset(words, 0, wordCount(bitCount) * sizeof(uint64_t)); }

    /**
     * Lowest bit at or after from whose value is value (set if true, clear if false): writes its index to
     * index and returns true, or returns false if none remain. Enumerate by advancing from one past each hit.
     */
    bool findBit(uint32_t from, bool value, uint32_t * index) const noexcept;

private:
    static constexpr uint32_t WORD_BITS = 64;

    static constexpr uint64_t ZERO = 0;
    static constexpr uint64_t ONE = 1;

    uint64_t * words;
    uint32_t bitCount;
};

} // namespace silk
