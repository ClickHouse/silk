#include <silk/util/bitmap.h>

#include <bit>
#include <cstdint>

namespace silk
{

bool Bitmap::findBit(uint32_t from, bool value, uint32_t * index) const noexcept
{
    uint32_t totalWords = wordCount(bitCount);
    uint64_t mask = ~ZERO << (from % WORD_BITS);

    for (uint32_t word = from / WORD_BITS; word < totalWords; ++word)
    {
        uint64_t matches = (value ? words[word] : ~words[word]) & mask;
        if (matches)
        {
            uint32_t found = word * WORD_BITS + static_cast<uint32_t>(std::countr_zero(matches));
            if (found >= bitCount)
            {
                return false;
            }

            *index = found;
            return true;
        }

        mask = ~ZERO;
    }

    return false;
}

} // namespace silk
