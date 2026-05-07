file(GLOB FIBERS_SOURCES CONFIGURE_DEPENDS *.cpp)

add_library(silk-fibers ${FIBERS_SOURCES})
target_link_libraries(silk-fibers PUBLIC silk-util)
target_link_libraries(silk-fibers PRIVATE Boost::context Liburing::Liburing SystemTap::Sdt)

add_subdirectory(tests)
add_subdirectory(benchmarks)
