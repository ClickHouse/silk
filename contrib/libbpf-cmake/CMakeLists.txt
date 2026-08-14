find_library(LIBELF_LIBRARY NAMES elf)
find_path(LIBELF_INCLUDE_DIR libelf.h)
if(NOT LIBELF_LIBRARY OR NOT LIBELF_INCLUDE_DIR)
    message(STATUS "Skipping libbpf: libelf not found (install libelf-dev)")
    return()
endif()
find_package(ZLIB REQUIRED)

set(LIBBPF_SRC ${CMAKE_SOURCE_DIR}/contrib/libbpf)
set(LIBBPF_BIN ${CMAKE_BINARY_DIR}/libbpf-generated)

# Stage src/*.h under a bpf/ subdir so consumers can use <bpf/X.h>.
# The headers live directly in src/ in the repo but ship under bpf/ in
# the install layout that <bpf/...> includes assume.
file(MAKE_DIRECTORY ${LIBBPF_BIN}/include/bpf)
file(GLOB _libbpf_public_headers ${LIBBPF_SRC}/src/*.h)
foreach(_hdr IN LISTS _libbpf_public_headers)
    get_filename_component(_name ${_hdr} NAME)
    configure_file(${_hdr} ${LIBBPF_BIN}/include/bpf/${_name} COPYONLY)
endforeach()

file(GLOB LIBBPF_SOURCES ${LIBBPF_SRC}/src/*.c)

add_library(_libbpf STATIC ${LIBBPF_SOURCES})

target_compile_options(_libbpf PRIVATE
    -w
    -std=gnu89
    # Don't sanitize third-party libbpf source; lets bpftool (which we
    # also build with -fno-sanitize=all) link it without pulling in the
    # sanitizer runtime, and keeps silk's profiler binary slimmer.
    -fno-sanitize=all
)

target_compile_definitions(_libbpf PRIVATE
    _LARGEFILE64_SOURCE
    _FILE_OFFSET_BITS=64
)

target_include_directories(_libbpf
    PRIVATE ${LIBBPF_SRC}/src
    PUBLIC ${LIBBPF_BIN}/include ${LIBBPF_SRC}/include ${LIBBPF_SRC}/include/uapi
)

target_link_libraries(_libbpf PUBLIC ZLIB::ZLIB ${LIBELF_LIBRARY})
target_include_directories(_libbpf SYSTEM PUBLIC ${LIBELF_INCLUDE_DIR})

add_library(Libbpf::Libbpf ALIAS _libbpf)
