#include <silk/util/perf.h>

#include <gtest/gtest.h>

int main(int argc, char ** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    silk::initRseq();
    return RUN_ALL_TESTS();
}
