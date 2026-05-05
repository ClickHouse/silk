file(GLOB_RECURSE BENCH_SOURCES CONFIGURE_DEPENDS *.cpp)

add_executable(util-bench ${BENCH_SOURCES})
target_link_libraries(util-bench PRIVATE silk-util benchmark::benchmark)
