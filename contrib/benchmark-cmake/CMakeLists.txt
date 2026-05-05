get_directory_property(_opts COMPILE_OPTIONS)
list(FILTER _opts EXCLUDE REGEX "^-W")
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_opts}")

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark
    ${CMAKE_CURRENT_BINARY_DIR}/benchmark
    SYSTEM
)

# For MSan builds, libc++ must appear after benchmark in the link order so that
# benchmark's instantiated template references (e.g. regex_error) can be resolved.
# PRIVATE keeps cxx out of the install export.
if(TARGET cxx)
    target_link_libraries(benchmark PRIVATE cxx)
    target_link_libraries(benchmark_main PRIVATE cxx)
endif()
