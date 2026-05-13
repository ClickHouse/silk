cmake_minimum_required(VERSION 3.28)
project(Silk LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "This project requires Clang (got ${CMAKE_CXX_COMPILER_ID}). Use: ./bb configure")
endif()

get_filename_component(_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
list(APPEND CMAKE_PROGRAM_PATH "${_compiler_dir}")

find_program(CCACHE ccache)
if(CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE}")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Symlink compile_commands.json to the source root so clangd picks it up
execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
    ${CMAKE_BINARY_DIR}/compile_commands.json
    ${CMAKE_SOURCE_DIR}/compile_commands.json
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# Boost headers only - context is vendored under contrib/fcontext.
find_package(Boost REQUIRED)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    add_compile_options(-march=x86-64-v3)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    add_compile_options(-march=armv8.2-a+lse)
endif()

add_compile_options(
    -Wall
    -Wextra
    -Werror
    -fno-omit-frame-pointer
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}/=
    -ffile-prefix-map=${CMAKE_BINARY_DIR}=
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-ggdb3>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-g>
)

set(SANITIZER "" CACHE STRING "Sanitizer to enable: thread, address, undefined, or memory")
if(SANITIZER)
    add_compile_options(-fsanitize=${SANITIZER})
    add_link_options(-fsanitize=${SANITIZER})
    if(SANITIZER STREQUAL "undefined")
        add_compile_options(-fno-sanitize=function -fno-sanitize-recover=undefined)
    elseif(SANITIZER STREQUAL "memory")
        add_compile_options(-fsanitize-memory-track-origins=2)
    endif()
endif()

if(SANITIZER STREQUAL "memory")
    # These must come before any add_subdirectory so that cxxabi, cxx, googletest,
    # and benchmark all inherit the libc++ include paths and -nostdinc++ flag.
    include_directories(SYSTEM
        "${CMAKE_BINARY_DIR}/contrib/libcxx-cmake/include/c++/v1"
        "${CMAKE_SOURCE_DIR}/contrib/llvm-project/libcxx/include"
        "${CMAKE_SOURCE_DIR}/contrib/llvm-project/libcxxabi/include"
    )
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>)
    add_link_options(-nostdlib++)
    # Build libc++ and libc++abi from source so they inherit -fsanitize=memory.
    add_subdirectory(contrib/libcxxabi-cmake)
    add_subdirectory(contrib/libcxx-cmake)
    # benchmark and googletest call export() on their targets, which requires all
    # transitive dependencies to be in some export set. Registering cxx/cxxabi in
    # a local file export satisfies that check without installing anything.
    export(TARGETS cxx cxxabi FILE "${CMAKE_BINARY_DIR}/cxx-targets.cmake")
endif()

add_subdirectory(contrib/benchmark-cmake)
add_subdirectory(contrib/cxxopts-cmake)
add_subdirectory(contrib/fcontext)
add_subdirectory(contrib/googletest-cmake)
# libbpf-cmake must precede bpftool-cmake: bpftool's wrapper early-exits if
# Libbpf::Libbpf is not yet a target.
add_subdirectory(contrib/libbpf-cmake)
add_subdirectory(contrib/bpftool-cmake)
add_subdirectory(contrib/librseq-cmake)
add_subdirectory(contrib/liburing-cmake)
add_subdirectory(contrib/systemtap-sdt)

option(BUILD_LIBBACKTRACE "Build libbacktrace (used to symbolize stack traces on SILK_ASSERT failure and by the profiler)" ON)
if(BUILD_LIBBACKTRACE)
    add_subdirectory(contrib/libbacktrace-cmake)
endif()

option(BUILD_POCO "Build Poco library (required by http-perf)" OFF)
if(BUILD_POCO)
    add_subdirectory(contrib/poco-cmake)
endif()

option(BUILD_AWS "Build AWS SDK (required by s3-perf)" OFF)
if(BUILD_AWS)
    add_subdirectory(contrib/aws-sdk-cpp-cmake)
endif()

option(BUILD_JEMALLOC "Build jemalloc library" OFF)
if(BUILD_JEMALLOC)
    add_subdirectory(contrib/jemalloc-cmake)
endif()

if(SANITIZER STREQUAL "memory")
    # Called after third-party subdirs so their export mechanisms don't see cxx
    # as a dependency. Only our own targets (added below) inherit this.
    link_libraries(cxx)
endif()

option(COVERAGE "Enable coverage instrumentation" OFF)
if(COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
endif()

enable_testing()
include(GoogleTest)

include_directories(include)
include_directories(src)

add_subdirectory(src/util)
add_subdirectory(src/fibers)
add_subdirectory(src/gdb)

# Perf and profiler targets may pull in system libs with libstdc++ ABI
# (Poco, AWS SDK) that conflict with the instrumented libc++ used in MSan
# builds.
if(NOT SANITIZER STREQUAL "memory")
    add_subdirectory(src/perf)
    if(BUILD_LIBBACKTRACE)
        # symbolizer.cpp depends directly on <backtrace.h>.
        add_subdirectory(src/profiler)
    endif()
endif()
