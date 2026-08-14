if(NOT TARGET Libbpf::Libbpf)
    return()
endif()

set(BPFTOOL_SRC ${CMAKE_SOURCE_DIR}/contrib/bpftool)

file(GLOB BPFTOOL_SOURCES ${BPFTOOL_SRC}/src/*.c)
list(APPEND BPFTOOL_SOURCES ${BPFTOOL_SRC}/src/kernel/bpf/disasm.c)
# jit_disasm.c needs libbfd or LLVM disassembler; without either the
# inline stubs in main.h cover the API for everything else we use.
list(REMOVE_ITEM BPFTOOL_SOURCES ${BPFTOOL_SRC}/src/jit_disasm.c)

add_executable(bpftool ${BPFTOOL_SOURCES})

target_compile_options(bpftool PRIVATE
    -w
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    # struct list_head lives in <linux/types.h>; libbpf's <linux/list.h>
    # uses it without including types.h directly.
    -include linux/types.h
    # bpftool is a build-time tool; don't pay the cost of sanitizers,
    # and skip false positives like qsort_r(NULL, 0, ...) tripping
    # UBSan's nonnull-attribute check.
    -fno-sanitize=all
)
target_link_options(bpftool PRIVATE -fno-sanitize=all)

# BPFTOOL_WITHOUT_SKELETONS turns the prog-profile / pid-iter paths (which
# depend on bpftool's own bootstrap-built skel headers) into stubs. The
# remaining commands -- including `btf dump` and `gen skeleton` that silk
# uses -- compile without needing a bootstrap stage.
target_compile_definitions(bpftool PRIVATE
    BPFTOOL_WITHOUT_SKELETONS
    BPFTOOL_VERSION="7.5.0"
    PACKAGE="bpftool"
    __EXPORTED_HEADERS__
    _GNU_SOURCE
)

target_include_directories(bpftool PRIVATE
    ${BPFTOOL_SRC}/include
    ${BPFTOOL_SRC}/include/uapi
    ${BPFTOOL_SRC}/src/kernel/bpf
)

target_link_libraries(bpftool PRIVATE Libbpf::Libbpf)
