set(CXXOPTS_SRC ${CMAKE_SOURCE_DIR}/contrib/cxxopts)

add_library(cxxopts INTERFACE)
target_include_directories(cxxopts SYSTEM INTERFACE ${CXXOPTS_SRC}/include)

add_library(cxxopts::cxxopts ALIAS cxxopts)
