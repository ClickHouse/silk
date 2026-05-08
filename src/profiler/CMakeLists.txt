if(NOT TARGET Libbpf::Libbpf OR NOT TARGET bpftool)
    message(STATUS "Skipping profiler: bundled libbpf/bpftool unavailable (libelf-dev missing?)")
    return()
endif()

find_program(LLVM_STRIP NAMES llvm-strip llvm-strip-21)
find_program(CLANG NAMES clang clang-21)
if(NOT LLVM_STRIP OR NOT CLANG)
    message(STATUS "Skipping profiler: missing llvm-strip=${LLVM_STRIP} clang=${CLANG}")
    return()
endif()

set(BPFTOOL "$<TARGET_FILE:bpftool>")

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(BPF_ARCH "x86")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(BPF_ARCH "arm64")
else()
    message(FATAL_ERROR "Unsupported architecture for BPF profiler: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(VMLINUX_H ${CMAKE_CURRENT_BINARY_DIR}/vmlinux.h)
set(BPF_OBJ ${CMAKE_CURRENT_BINARY_DIR}/profiler.bpf.o)
set(BPF_SKEL ${CMAKE_CURRENT_BINARY_DIR}/profiler.skel.h)

# Use cmake -P scripts to avoid shell redirect escaping issues in ninja rules.
# file(GENERATE) (vs WRITE) so $<TARGET_FILE:bpftool> resolves at generate time.
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gen_vmlinux.cmake"
    CONTENT
"execute_process(
    COMMAND \"${BPFTOOL}\" btf dump file /sys/kernel/btf/vmlinux format c
    OUTPUT_FILE \"${VMLINUX_H}\"
    COMMAND_ERROR_IS_FATAL ANY
)
")

file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gen_skeleton.cmake"
    CONTENT
"execute_process(
    COMMAND \"${BPFTOOL}\" gen skeleton \"${BPF_OBJ}\"
    OUTPUT_FILE \"${BPF_SKEL}\"
    COMMAND_ERROR_IS_FATAL ANY
)
")

add_custom_command(
    OUTPUT ${VMLINUX_H}
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/gen_vmlinux.cmake
    DEPENDS bpftool
    COMMENT "Generating vmlinux.h"
)

# usdt.bpf.h transitively pulls in <linux/errno.h> -> <asm/errno.h>, which
# lives in the multiarch include path on Debian/Ubuntu. Probe both x86_64 and
# aarch64 multiarch dirs; the architecture mismatch path is harmless if absent.
add_custom_command(
    OUTPUT ${BPF_OBJ}
    COMMAND ${CLANG}
    --target=bpf
    -g -O2
    -D__TARGET_ARCH_${BPF_ARCH}
    -I${CMAKE_CURRENT_BINARY_DIR}
    -I${CMAKE_BINARY_DIR}/libbpf-generated/include
    -I/usr/include/x86_64-linux-gnu
    -I/usr/include/aarch64-linux-gnu
    -c ${CMAKE_CURRENT_SOURCE_DIR}/profiler.bpf.c
    -o ${BPF_OBJ}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/profiler.bpf.c ${VMLINUX_H}
    COMMENT "Compiling BPF program"
    VERBATIM
)

add_custom_command(
    OUTPUT ${BPF_SKEL}
    # Strip DWARF in-place (keeps BTF); bpftool then derives struct name from profiler.bpf.o -> profiler_bpf.
    COMMAND ${LLVM_STRIP} -g ${BPF_OBJ}
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/gen_skeleton.cmake
    DEPENDS ${BPF_OBJ} bpftool
    COMMENT "Generating BPF skeleton"
    VERBATIM
)

add_library(symbolizer STATIC symbolizer.cpp)
target_link_libraries(symbolizer PUBLIC silk-util Libbacktrace::Libbacktrace)

add_custom_target(profiler-skel DEPENDS ${BPF_SKEL})

add_executable(profiler main.cpp profiler.cpp)
add_dependencies(profiler profiler-skel)

target_include_directories(profiler PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(profiler PRIVATE Libbpf::Libbpf symbolizer Boost::program_options)

add_subdirectory(tests)
