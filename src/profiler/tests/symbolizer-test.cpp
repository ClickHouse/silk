#include <profiler/symbolizer.h>

#include <gtest/gtest.h>

#include <unistd.h>

// Non-static so the symbol is exported to .dynsym, which backtrace_syminfo
// reads on all platforms and build configurations (including coverage).
__attribute__((noinline)) void knownFunction()
{
}

TEST(Symbolizer, ReadMappingsSucceeds)
{
    Symbolizer sym;

    int r = sym.readSelfMappings();
    EXPECT_EQ(r, 0);

    r = sym.readMappings(::getpid());
    EXPECT_EQ(r, 0);
}

TEST(Symbolizer, ResolveKnownFunction)
{
    Symbolizer sym;

    int r = sym.readSelfMappings();
    ASSERT_EQ(r, 0);

    r = sym.readMappings(::getpid());
    ASSERT_EQ(r, 0);

    uint64_t addr = reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&knownFunction));
    const std::string & name = sym.resolve(addr);

    EXPECT_NE(name.find("knownFunction"), std::string::npos);
}

TEST(Symbolizer, ResolveUnmappedAddressReturnsHex)
{
    Symbolizer sym;

    int r = sym.readSelfMappings();
    ASSERT_EQ(r, 0);

    r = sym.readMappings(::getpid());
    ASSERT_EQ(r, 0);

    const std::string & name = sym.resolve(0x1234);
    EXPECT_EQ(name, "0x1234");
}

TEST(Symbolizer, CacheReturnsSameReference)
{
    Symbolizer sym;

    int r = sym.readSelfMappings();
    ASSERT_EQ(r, 0);

    r = sym.readMappings(::getpid());
    ASSERT_EQ(r, 0);

    uint64_t addr = reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&knownFunction));
    const std::string & first = sym.resolve(addr);
    const std::string & second = sym.resolve(addr);

    EXPECT_EQ(&first, &second);
}
