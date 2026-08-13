file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS *.cpp)

# fiber-cpuset-test is its own executable: it needs a main that restricts the scheduler
# to a CPU subset before running, so it cannot share fibers-test's global init.
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/fiber-cpuset-test.cpp)

add_executable(fibers-test ${TEST_SOURCES})
target_link_libraries(fibers-test PRIVATE silk-fibers GTest::gtest)

# Each fiber test initializes one io_uring instance per CPU, quickly exhausting
# the per-process memlock limit when many tests run concurrently. Serializing
# them avoids the limit without affecting the (io_uring-free) util tests.
# On a test timeout ctest sends SIGQUIT (caught by the crash-dumper installed in main) and waits the grace
# period for the gdb dump, instead of the default uncatchable SIGKILL.
gtest_discover_tests(fibers-test
    PROPERTIES RESOURCE_LOCK iouring TIMEOUT_SIGNAL_NAME SIGQUIT TIMEOUT_SIGNAL_GRACE_PERIOD 60)

add_executable(fibers-cpuset-test fiber-cpuset-test.cpp)
target_link_libraries(fibers-cpuset-test PRIVATE silk-fibers GTest::gtest)
gtest_discover_tests(fibers-cpuset-test
    PROPERTIES RESOURCE_LOCK iouring TIMEOUT_SIGNAL_NAME SIGQUIT TIMEOUT_SIGNAL_GRACE_PERIOD 60)
