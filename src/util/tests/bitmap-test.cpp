#include <silk/util/bitmap.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace silk
{

// Enumerate every bit equal to value by repeatedly advancing past each hit, exactly as a caller would.
static std::vector<uint32_t> collectBits(const Bitmap & bitmap, bool value)
{
    std::vector<uint32_t> result;

    for (uint32_t bit = 0; bitmap.findBit(bit, value, &bit); ++bit)
    {
        result.push_back(bit);
    }

    return result;
}

// wordCount rounds bitCount up to whole 64-bit words.
TEST(BitmapTest, WordCount)
{
    struct Case
    {
        uint32_t bitCount;
        uint32_t expected;
    };

    const Case cases[] = {
        {0, 0},
        {1, 1},
        {64, 1},
        {65, 2},
        {128, 2},
        {129, 3},
        {UINT32_MAX - 63, 67'108'863},
        {UINT32_MAX - 62, 67'108'864},
        {UINT32_MAX, 67'108'864},
    };

    for (const Case & testCase : cases)
    {
        uint32_t count = Bitmap::wordCount(testCase.bitCount);
        ASSERT_EQ(count, testCase.expected) << "bitCount=" << testCase.bitCount;
    }
}

// set / test / clear address single bits, including the word-boundary bits 63 and 64, without disturbing neighbours.
TEST(BitmapTest, SetTestAndClearIndividualBits)
{
    uint64_t words[2] = {};
    Bitmap bitmap(words, 128);

    const uint32_t indices[] = {0, 63, 64, 127};

    for (uint32_t index : indices)
    {
        bool before = bitmap.test(index);
        ASSERT_FALSE(before) << "index=" << index;
    }

    for (uint32_t index : indices)
    {
        bitmap.set(index);
    }

    for (uint32_t index : indices)
    {
        bool after = bitmap.test(index);
        ASSERT_TRUE(after) << "index=" << index;
    }

    bool neighbourLow = bitmap.test(1);
    ASSERT_FALSE(neighbourLow);

    bool neighbourHigh = bitmap.test(65);
    ASSERT_FALSE(neighbourHigh);

    bitmap.clear(64);

    bool cleared = bitmap.test(64);
    ASSERT_FALSE(cleared);

    bool stillSet = bitmap.test(63);
    ASSERT_TRUE(stillSet);
}

// The no-argument clear zeroes every word.
TEST(BitmapTest, ClearZeroesEveryBit)
{
    uint64_t words[2] = {};
    Bitmap bitmap(words, 128);

    bitmap.set(0);
    bitmap.set(70);
    bitmap.set(127);

    bitmap.clear();

    std::vector<uint32_t> remaining = collectBits(bitmap, true);
    bool empty = remaining.empty();
    ASSERT_TRUE(empty);
}

// findBit walks set bits in ascending order, crossing the word boundary.
TEST(BitmapTest, FindEnumeratesSetBitsInOrderAcrossWords)
{
    uint64_t words[2] = {};
    Bitmap bitmap(words, 128);

    const std::vector<uint32_t> expected = {0, 63, 64, 65, 127};

    for (uint32_t index : expected)
    {
        bitmap.set(index);
    }

    std::vector<uint32_t> found = collectBits(bitmap, true);
    ASSERT_EQ(found, expected);
}

// Searching for value false enumerates the clear bits.
TEST(BitmapTest, FindEnumeratesClearBits)
{
    uint64_t words[1] = {};
    Bitmap bitmap(words, 8);

    for (uint32_t index = 0; index < 8; ++index)
    {
        bitmap.set(index);
    }

    bitmap.clear(1);
    bitmap.clear(4);
    bitmap.clear(7);

    std::vector<uint32_t> found = collectBits(bitmap, false);
    const std::vector<uint32_t> expected = {1, 4, 7};
    ASSERT_EQ(found, expected);
}

// findBit starts at from inclusive, skipping any earlier matching bit.
TEST(BitmapTest, FindBitHonoursFromLowerBound)
{
    uint64_t words[1] = {};
    Bitmap bitmap(words, 64);

    bitmap.set(2);
    bitmap.set(10);
    bitmap.set(40);

    uint32_t index;

    bool fromZero = bitmap.findBit(0, true, &index);
    ASSERT_TRUE(fromZero);
    ASSERT_EQ(index, 2u);

    bool fromThree = bitmap.findBit(3, true, &index);
    ASSERT_TRUE(fromThree);
    ASSERT_EQ(index, 10u);

    bool fromTen = bitmap.findBit(10, true, &index);
    ASSERT_TRUE(fromTen);
    ASSERT_EQ(index, 10u);

    bool fromEleven = bitmap.findBit(11, true, &index);
    ASSERT_TRUE(fromEleven);
    ASSERT_EQ(index, 40u);
}

// findBit returns false when nothing matches at or after from.
TEST(BitmapTest, FindBitReturnsFalseWhenNoMatchRemains)
{
    uint64_t words[2] = {};
    Bitmap bitmap(words, 128);

    bitmap.set(5);

    uint32_t index;

    bool pastLast = bitmap.findBit(6, true, &index);
    ASSERT_FALSE(pastLast);

    bool atEnd = bitmap.findBit(128, true, &index);
    ASSERT_FALSE(atEnd);

    bitmap.clear();

    bool none = bitmap.findBit(0, true, &index);
    ASSERT_FALSE(none);
}

// bitCount 70 spans two words; bits 70..127 are don't-care padding that read as zero. A clear-bit search
// must not report them - only bits below bitCount count.
TEST(BitmapTest, FindClearIgnoresBitsBeyondBitCount)
{
    uint64_t words[2] = {};
    Bitmap bitmap(words, 70);

    for (uint32_t position = 0; position < 70; ++position)
    {
        bitmap.set(position);
    }

    uint32_t index;

    bool foundPadding = bitmap.findBit(0, false, &index);
    ASSERT_FALSE(foundPadding);

    bitmap.clear(68);

    bool foundReal = bitmap.findBit(0, false, &index);
    ASSERT_TRUE(foundReal);
    ASSERT_EQ(index, 68u);
}

} // namespace silk
