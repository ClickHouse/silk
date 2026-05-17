#include <silk/util/scope-guard.h>

#include <gtest/gtest.h>

namespace silk
{

TEST(ScopeGuardTest, RunsOnNormalExit)
{
    int counter = 0;
    {
        SILK_SCOPE_EXIT
        {
            ++counter;
        };
        EXPECT_EQ(counter, 0);
    }
    EXPECT_EQ(counter, 1);
}

TEST(ScopeGuardTest, RunsInReverseOrder)
{
    int sequence[3] = {0, 0, 0};
    int next = 0;
    {
        SILK_SCOPE_EXIT
        {
            sequence[2] = next++;
        };
        SILK_SCOPE_EXIT
        {
            sequence[1] = next++;
        };
        SILK_SCOPE_EXIT
        {
            sequence[0] = next++;
        };
    }
    EXPECT_EQ(sequence[0], 0);
    EXPECT_EQ(sequence[1], 1);
    EXPECT_EQ(sequence[2], 2);
}

TEST(ScopeGuardTest, MultiStatementBody)
{
    int a = 0;
    int b = 0;
    {
        SILK_SCOPE_EXIT
        {
            a = 1;
            b = 2;
        };
    }
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);
}

TEST(ScopeGuardTest, CapturesByReference)
{
    int * heap = new int(7);
    bool freed = false;
    {
        SILK_SCOPE_EXIT
        {
            delete heap;
            freed = true;
        };
        EXPECT_FALSE(freed);
    }
    EXPECT_TRUE(freed);
}

} // namespace silk
