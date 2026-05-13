file(GLOB_RECURSE BENCH_SOURCES CONFIGURE_DEPENDS *.cpp)

add_executable(fibers-bench ${BENCH_SOURCES})
target_link_libraries(fibers-bench PRIVATE silk-fibers Fcontext::Fcontext benchmark::benchmark)
