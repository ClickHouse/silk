file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS *.cpp)

add_executable(util-test ${TEST_SOURCES})
target_link_libraries(util-test PRIVATE silk-util GTest::gtest)

gtest_discover_tests(util-test)
