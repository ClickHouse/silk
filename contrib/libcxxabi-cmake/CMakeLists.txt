get_directory_property(_opts COMPILE_OPTIONS)
list(FILTER _opts EXCLUDE REGEX "^-W")
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_opts}")

set(LIBCXXABI_SOURCE_DIR "${CMAKE_SOURCE_DIR}/contrib/llvm-project/libcxxabi")

set(SRCS
    "${LIBCXXABI_SOURCE_DIR}/src/abort_message.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/cxa_aux_runtime.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/cxa_default_handlers.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/cxa_demangle.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/cxa_exception.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/cxa_exception_storage.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/cxa_guard.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/cxa_handlers.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/cxa_personality.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/cxa_thread_atexit.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/cxa_vector.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/cxa_virtual.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/fallback_malloc.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/private_typeinfo.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/stdlib_exception.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/stdlib_new_delete.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/stdlib_stdexcept.cpp"
    "${LIBCXXABI_SOURCE_DIR}/src/stdlib_typeinfo.cpp"
)

add_library(cxxabi STATIC ${SRCS})

target_include_directories(cxxabi SYSTEM BEFORE
    PUBLIC "${LIBCXXABI_SOURCE_DIR}/include"
    PRIVATE "${LIBCXXABI_SOURCE_DIR}/../libcxx/include"
    PRIVATE "${LIBCXXABI_SOURCE_DIR}/../libcxx/src"
)

target_compile_definitions(cxxabi PRIVATE _LIBCPP_BUILDING_LIBRARY HAS_THREAD_LOCAL)

target_compile_options(cxxabi PRIVATE
    -nostdinc++
    -fno-sanitize=undefined
    -fomit-frame-pointer
    -momit-leaf-frame-pointer
)

target_link_libraries(cxxabi PUBLIC gcc_s)
