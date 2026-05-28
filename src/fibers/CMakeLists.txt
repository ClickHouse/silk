file(GLOB FIBERS_SOURCES CONFIGURE_DEPENDS *.cpp)

add_library(silk-fibers ${FIBERS_SOURCES})
target_link_libraries(silk-fibers PUBLIC silk-util)
target_link_libraries(silk-fibers PRIVATE Fcontext::Fcontext Liburing::Liburing)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
