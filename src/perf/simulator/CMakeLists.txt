file(GLOB SIMULATOR_SOURCES CONFIGURE_DEPENDS *.cpp)

add_executable(fibers-simulator ${SIMULATOR_SOURCES})
target_link_libraries(fibers-simulator PRIVATE silk-perf cxxopts::cxxopts)
