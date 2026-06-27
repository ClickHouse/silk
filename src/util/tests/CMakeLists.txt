file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS *.cpp)

add_executable(util-test ${TEST_SOURCES})
target_link_libraries(util-test PRIVATE silk-util GTest::gtest)

# On a test timeout ctest sends SIGQUIT (caught by the crash-dumper installed in main) and waits the grace
# period for the gdb dump, instead of the default uncatchable SIGKILL.
gtest_discover_tests(util-test
    PROPERTIES TIMEOUT_SIGNAL_NAME SIGQUIT TIMEOUT_SIGNAL_GRACE_PERIOD 60)
