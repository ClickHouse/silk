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

add_subdirectory(tests)
add_subdirectory(benchmarks)
