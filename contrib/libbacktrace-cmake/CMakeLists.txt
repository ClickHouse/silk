set(LIBBACKTRACE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../libbacktrace)
set(LIBBACKTRACE_BIN ${CMAKE_BINARY_DIR}/libbacktrace-generated)
file(MAKE_DIRECTORY ${LIBBACKTRACE_BIN})

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
    ${LIBBACKTRACE_BIN}/config.h
)

set(BACKTRACE_SUPPORTED 1)
set(BACKTRACE_USES_MALLOC 0)
set(BACKTRACE_SUPPORTS_THREADS 1)
set(BACKTRACE_SUPPORTS_DATA 1)

configure_file(
    ${LIBBACKTRACE_SRC}/backtrace-supported.h.in
    ${LIBBACKTRACE_BIN}/backtrace-supported.h
    @ONLY
)

execute_process(
    COMMAND patch -o ${LIBBACKTRACE_BIN}/elf.c ${LIBBACKTRACE_SRC}/elf.c
    INPUT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/patches/elf.patch
    RESULT_VARIABLE _patch_result
)
if(_patch_result GREATER 1)
    message(FATAL_ERROR "Failed to patch libbacktrace/elf.c (exit code ${_patch_result})")
endif()

add_library(libbacktrace STATIC
    ${LIBBACKTRACE_SRC}/atomic.c
    ${LIBBACKTRACE_SRC}/backtrace.c
    ${LIBBACKTRACE_SRC}/dwarf.c
    ${LIBBACKTRACE_BIN}/elf.c
    ${LIBBACKTRACE_SRC}/fileline.c
    ${LIBBACKTRACE_SRC}/mmap.c
    ${LIBBACKTRACE_SRC}/mmapio.c
    ${LIBBACKTRACE_SRC}/posix.c
    ${LIBBACKTRACE_SRC}/print.c
    ${LIBBACKTRACE_SRC}/sort.c
    ${LIBBACKTRACE_SRC}/state.c
)

target_include_directories(libbacktrace
    PRIVATE ${LIBBACKTRACE_BIN}
    PUBLIC ${LIBBACKTRACE_SRC}
)

add_library(Libbacktrace::Libbacktrace ALIAS libbacktrace)
