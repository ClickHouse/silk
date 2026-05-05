file(GLOB UTIL_SOURCES CONFIGURE_DEPENDS *.cpp)

add_library(silk-util ${UTIL_SOURCES})
target_link_libraries(silk-util PRIVATE Libbacktrace::Libbacktrace Librseq::Librseq)

add_subdirectory(tests)
add_subdirectory(benchmarks)
