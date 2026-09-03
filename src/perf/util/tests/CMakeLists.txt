file(GLOB TEST_SOURCES CONFIGURE_DEPENDS *.cpp)

add_executable(perf-util-test ${TEST_SOURCES})
target_link_libraries(perf-util-test PRIVATE silk-perf GTest::gtest_main)

gtest_discover_tests(perf-util-test)
