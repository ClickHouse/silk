set(CXXOPTS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../cxxopts)

add_library(cxxopts INTERFACE)
target_include_directories(cxxopts SYSTEM INTERFACE ${CXXOPTS_SRC}/include)

add_library(cxxopts::cxxopts ALIAS cxxopts)
