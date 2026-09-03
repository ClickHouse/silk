#include <perf/util/parse.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

TEST(ParseTest, SizeWithoutSuffix)
{
    uint64_t size = parseSize("4096");
    ASSERT_EQ(size, 4096ULL);
}

TEST(ParseTest, SizeSuffixes)
{
    uint64_t kilobytes = parseSize("4k");
    ASSERT_EQ(kilobytes, 4ULL * 1024);

    uint64_t kilobytesUpper = parseSize("4K");
    ASSERT_EQ(kilobytesUpper, 4ULL * 1024);

    uint64_t megabytes = parseSize("4m");
    ASSERT_EQ(megabytes, 4ULL * 1024 * 1024);

    uint64_t megabytesUpper = parseSize("4M");
    ASSERT_EQ(megabytesUpper, 4ULL * 1024 * 1024);

    uint64_t gigabytes = parseSize("4g");
    ASSERT_EQ(gigabytes, 4ULL * 1024 * 1024 * 1024);

    uint64_t gigabytesUpper = parseSize("4G");
    ASSERT_EQ(gigabytesUpper, 4ULL * 1024 * 1024 * 1024);
}

TEST(ParseTest, SizeRejectsTrailingCharacters)
{
    ASSERT_THROW(parseSize("4kb"), std::invalid_argument);
    ASSERT_THROW(parseSize("4x"), std::invalid_argument);
    ASSERT_THROW(parseSize("1.5m"), std::invalid_argument);
    ASSERT_THROW(parseSize("12junk"), std::invalid_argument);
    ASSERT_THROW(parseSize("1024wat"), std::invalid_argument);
}

TEST(ParseTest, SizeRejectsOverflow)
{
    ASSERT_THROW(parseSize("18446744073709551616"), std::out_of_range);
    ASSERT_THROW(parseSize("17179869185g"), std::out_of_range);
}
