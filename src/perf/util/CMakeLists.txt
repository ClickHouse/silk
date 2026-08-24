file(GLOB UTIL_SOURCES CONFIGURE_DEPENDS *.cpp)

add_library(silk-perf STATIC ${UTIL_SOURCES})
target_link_libraries(silk-perf PUBLIC silk-fibers)
