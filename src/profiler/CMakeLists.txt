find_program(BPFTOOL bpftool)
find_program(LLVM_STRIP NAMES llvm-strip llvm-strip-21)
find_program(CLANG NAMES clang clang-21)
find_path(BPF_INCLUDE bpf/bpf_core_read.h)

if(NOT BPFTOOL OR NOT LLVM_STRIP OR NOT CLANG OR NOT BPF_INCLUDE)
    message(STATUS "Skipping profiler: missing bpftool=${BPFTOOL} llvm-strip=${LLVM_STRIP} clang=${CLANG} libbpf-dev=${BPF_INCLUDE}")
    return()
endif()

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
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen_vmlinux.cmake"
    "execute_process(\n"
    "    COMMAND \"${BPFTOOL}\" btf dump file /sys/kernel/btf/vmlinux format c\n"
    "    OUTPUT_FILE \"${VMLINUX_H}\"\n"
    "    COMMAND_ERROR_IS_FATAL ANY\n"
    ")\n"
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen_skeleton.cmake"
    "execute_process(\n"
    "    COMMAND \"${BPFTOOL}\" gen skeleton \"${BPF_OBJ}\"\n"
    "    OUTPUT_FILE \"${BPF_SKEL}\"\n"
    "    COMMAND_ERROR_IS_FATAL ANY\n"
    ")\n"
)

add_custom_command(
    OUTPUT ${VMLINUX_H}
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/gen_vmlinux.cmake
    COMMENT "Generating vmlinux.h"
)

add_custom_command(
    OUTPUT ${BPF_OBJ}
    COMMAND ${CLANG}
    --target=bpf
    -g -O2
    -D__TARGET_ARCH_${BPF_ARCH}
    -I${CMAKE_CURRENT_BINARY_DIR}
    -I/usr/include/bpf
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
    DEPENDS ${BPF_OBJ}
    COMMENT "Generating BPF skeleton"
    VERBATIM
)

add_library(symbolizer STATIC symbolizer.cpp)
target_link_libraries(symbolizer PUBLIC silk-util Libbacktrace::Libbacktrace)

add_custom_target(profiler-skel DEPENDS ${BPF_SKEL})

add_executable(profiler main.cpp profiler.cpp)
add_dependencies(profiler profiler-skel)

target_include_directories(profiler PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(profiler PRIVATE bpf elf z symbolizer Boost::program_options)

add_subdirectory(tests)
