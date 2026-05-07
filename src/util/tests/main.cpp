#include <silk/util/init.h>

#include <gtest/gtest.h>

int main(int argc, char ** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    if (::testing::GTEST_FLAG(list_tests))
    {
        return RUN_ALL_TESTS();
    }

    silk::initialize();

    int r = RUN_ALL_TESTS();

    silk::destroy();
    return r;
}
