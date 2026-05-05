add_executable(profiler-test symbolizer-test.cpp)
target_link_libraries(profiler-test PRIVATE symbolizer GTest::gtest_main)

gtest_discover_tests(profiler-test)
