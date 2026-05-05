get_directory_property(_opts COMPILE_OPTIONS)
list(FILTER _opts EXCLUDE REGEX "^-W")
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_opts}")

set(LIBCXX_SOURCE_DIR "${CMAKE_SOURCE_DIR}/contrib/llvm-project/libcxx")
set(LIBCXX_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include/c++/v1")
file(MAKE_DIRECTORY "${LIBCXX_GENERATED_INCLUDE_DIR}")

# Generate __config_site for a standard Linux x86_64 pthreads build.
# Variables not set here are left as /* #undef */ and pick up libc++ source defaults.
set(_LIBCPP_ABI_FORCE_ITANIUM 0)
set(_LIBCPP_ABI_FORCE_MICROSOFT 0)
set(_LIBCPP_HAS_THREADS 1)
set(_LIBCPP_HAS_MONOTONIC_CLOCK 1)
set(_LIBCPP_HAS_TERMINAL 1)
set(_LIBCPP_HAS_MUSL_LIBC 0)
set(_LIBCPP_HAS_THREAD_API_PTHREAD 1)
set(_LIBCPP_HAS_THREAD_API_EXTERNAL 0)
set(_LIBCPP_HAS_THREAD_API_WIN32 0)
set(_LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0)
set(_LIBCPP_HAS_FILESYSTEM 1)
set(_LIBCPP_HAS_RANDOM_DEVICE 1)
set(_LIBCPP_HAS_LOCALIZATION 1)
set(_LIBCPP_HAS_UNICODE 1)
set(_LIBCPP_HAS_WIDE_CHARACTERS 1)
set(_LIBCPP_HAS_TIME_ZONE_DATABASE 1)
set(_LIBCPP_INSTRUMENTED_WITH_ASAN 0)
set(_LIBCPP_HARDENING_MODE_DEFAULT _LIBCPP_HARDENING_MODE_NONE)
set(_LIBCPP_PSTL_BACKEND_SERIAL ON)
set(_LIBCPP_ABI_DEFINES "")
set(_LIBCPP_EXTRA_SITE_DEFINES "")
configure_file(
    "${LIBCXX_SOURCE_DIR}/include/__config_site.in"
    "${LIBCXX_GENERATED_INCLUDE_DIR}/__config_site"
    @ONLY
)

# __assertion_handler is a vendor-provided extension point; copy the LLVM default.
configure_file(
    "${LIBCXX_SOURCE_DIR}/vendor/llvm/default_assertion_handler.in"
    "${LIBCXX_GENERATED_INCLUDE_DIR}/__assertion_handler"
    COPYONLY
)

set(SRCS
    "${LIBCXX_SOURCE_DIR}/src/algorithm.cpp"
    "${LIBCXX_SOURCE_DIR}/src/any.cpp"
    "${LIBCXX_SOURCE_DIR}/src/atomic.cpp"
    "${LIBCXX_SOURCE_DIR}/src/barrier.cpp"
    "${LIBCXX_SOURCE_DIR}/src/bind.cpp"
    "${LIBCXX_SOURCE_DIR}/src/call_once.cpp"
    "${LIBCXX_SOURCE_DIR}/src/charconv.cpp"
    "${LIBCXX_SOURCE_DIR}/src/chrono.cpp"
    "${LIBCXX_SOURCE_DIR}/src/condition_variable.cpp"
    "${LIBCXX_SOURCE_DIR}/src/condition_variable_destructor.cpp"
    "${LIBCXX_SOURCE_DIR}/src/error_category.cpp"
    "${LIBCXX_SOURCE_DIR}/src/exception.cpp"
    "${LIBCXX_SOURCE_DIR}/src/expected.cpp"
    "${LIBCXX_SOURCE_DIR}/src/experimental/chrono_exception.cpp"
    "${LIBCXX_SOURCE_DIR}/src/experimental/keep.cpp"
    "${LIBCXX_SOURCE_DIR}/src/experimental/log_hardening_failure.cpp"
    "${LIBCXX_SOURCE_DIR}/src/experimental/time_zone.cpp"
    "${LIBCXX_SOURCE_DIR}/src/experimental/tzdb.cpp"
    "${LIBCXX_SOURCE_DIR}/src/experimental/tzdb_list.cpp"
    "${LIBCXX_SOURCE_DIR}/src/filesystem/directory_entry.cpp"
    "${LIBCXX_SOURCE_DIR}/src/filesystem/directory_iterator.cpp"
    "${LIBCXX_SOURCE_DIR}/src/filesystem/filesystem_clock.cpp"
    "${LIBCXX_SOURCE_DIR}/src/filesystem/filesystem_error.cpp"
    "${LIBCXX_SOURCE_DIR}/src/filesystem/int128_builtins.cpp"
    "${LIBCXX_SOURCE_DIR}/src/filesystem/operations.cpp"
    "${LIBCXX_SOURCE_DIR}/src/filesystem/path.cpp"
    "${LIBCXX_SOURCE_DIR}/src/fstream.cpp"
    "${LIBCXX_SOURCE_DIR}/src/functional.cpp"
    "${LIBCXX_SOURCE_DIR}/src/future.cpp"
    "${LIBCXX_SOURCE_DIR}/src/hash.cpp"
    "${LIBCXX_SOURCE_DIR}/src/ios.cpp"
    "${LIBCXX_SOURCE_DIR}/src/ios.instantiations.cpp"
    "${LIBCXX_SOURCE_DIR}/src/iostream.cpp"
    "${LIBCXX_SOURCE_DIR}/src/locale.cpp"
    "${LIBCXX_SOURCE_DIR}/src/memory.cpp"
    "${LIBCXX_SOURCE_DIR}/src/memory_resource.cpp"
    "${LIBCXX_SOURCE_DIR}/src/mutex.cpp"
    "${LIBCXX_SOURCE_DIR}/src/mutex_destructor.cpp"
    "${LIBCXX_SOURCE_DIR}/src/new.cpp"
    "${LIBCXX_SOURCE_DIR}/src/new_handler.cpp"
    "${LIBCXX_SOURCE_DIR}/src/new_helpers.cpp"
    "${LIBCXX_SOURCE_DIR}/src/optional.cpp"
    "${LIBCXX_SOURCE_DIR}/src/ostream.cpp"
    "${LIBCXX_SOURCE_DIR}/src/print.cpp"
    "${LIBCXX_SOURCE_DIR}/src/random.cpp"
    "${LIBCXX_SOURCE_DIR}/src/random_shuffle.cpp"
    "${LIBCXX_SOURCE_DIR}/src/regex.cpp"
    "${LIBCXX_SOURCE_DIR}/src/ryu/d2fixed.cpp"
    "${LIBCXX_SOURCE_DIR}/src/ryu/d2s.cpp"
    "${LIBCXX_SOURCE_DIR}/src/ryu/f2s.cpp"
    "${LIBCXX_SOURCE_DIR}/src/shared_mutex.cpp"
    "${LIBCXX_SOURCE_DIR}/src/stdexcept.cpp"
    "${LIBCXX_SOURCE_DIR}/src/string.cpp"
    "${LIBCXX_SOURCE_DIR}/src/strstream.cpp"
    "${LIBCXX_SOURCE_DIR}/src/system_error.cpp"
    "${LIBCXX_SOURCE_DIR}/src/thread.cpp"
    "${LIBCXX_SOURCE_DIR}/src/typeinfo.cpp"
    "${LIBCXX_SOURCE_DIR}/src/valarray.cpp"
    "${LIBCXX_SOURCE_DIR}/src/variant.cpp"
    "${LIBCXX_SOURCE_DIR}/src/vector.cpp"
    "${LIBCXX_SOURCE_DIR}/src/verbose_abort.cpp"
)

add_library(cxx STATIC ${SRCS})

# Generated headers first, then the source tree headers.
# LLVM libc shared/ headers (fp_bits.h etc.) are used by libcxx src files.
target_include_directories(cxx SYSTEM BEFORE
    PUBLIC "${LIBCXX_GENERATED_INCLUDE_DIR}"
    PUBLIC "${LIBCXX_SOURCE_DIR}/include"
    PRIVATE "${LIBCXX_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_SOURCE_DIR}/contrib/llvm-project/libc"
)

target_compile_definitions(cxx PRIVATE _LIBCPP_BUILDING_LIBRARY LIBCXX_BUILDING_LIBCXXABI)
target_compile_definitions(cxx PUBLIC _LIBCPP_REMOVE_TRANSITIVE_INCLUDES=1)

target_compile_options(cxx PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>)
target_compile_options(cxx PRIVATE -w)

target_link_libraries(cxx PUBLIC cxxabi)
