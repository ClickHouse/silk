file(GLOB UTIL_SOURCES CONFIGURE_DEPENDS *.cpp)

add_library(silk-util ${UTIL_SOURCES})
target_link_libraries(silk-util
    PUBLIC  Boost::headers
    PRIVATE Librseq::Librseq
)

if(BUILD_LIBBACKTRACE)
    target_compile_definitions(silk-util PRIVATE SILK_USE_LIBBACKTRACE)
    target_link_libraries(silk-util PRIVATE Libbacktrace::Libbacktrace)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
