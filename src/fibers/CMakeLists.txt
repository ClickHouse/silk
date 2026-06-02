file(GLOB FIBERS_SOURCES CONFIGURE_DEPENDS *.cpp)

add_library(silk-fibers ${FIBERS_SOURCES})
target_link_libraries(silk-fibers PUBLIC silk-util)
target_link_libraries(silk-fibers PRIVATE Fcontext::Fcontext Liburing::Liburing)

if(SILK_FIBER_LOCAL_STORAGE)
    target_compile_definitions(silk-fibers PUBLIC SILK_FIBER_LOCAL_STORAGE)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
