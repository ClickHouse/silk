file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS *.cpp)

add_executable(fibers-test ${TEST_SOURCES})
target_link_libraries(fibers-test PRIVATE silk-fibers GTest::gtest)

# Each fiber test initializes one io_uring instance per CPU, quickly exhausting
# the per-process memlock limit when many tests run concurrently. Serializing
# them avoids the limit without affecting the (io_uring-free) util tests.
gtest_discover_tests(fibers-test PROPERTIES RESOURCE_LOCK iouring)
