file(GLOB_RECURSE BENCH_SOURCES CONFIGURE_DEPENDS *.cpp)

# cpuset-bench is its own executable: it needs a main that restricts the scheduler
# to a CPU subset before running, so it cannot share fibers-bench's global init.
list(REMOVE_ITEM BENCH_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/cpuset-bench.cpp)

add_executable(fibers-bench ${BENCH_SOURCES})
target_link_libraries(fibers-bench PRIVATE silk-fibers Fcontext::Fcontext benchmark::benchmark)

add_executable(fibers-cpuset-bench cpuset-bench.cpp)
target_link_libraries(fibers-cpuset-bench PRIVATE silk-fibers Fcontext::Fcontext benchmark::benchmark)
