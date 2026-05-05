set(POCO_SRC ${CMAKE_SOURCE_DIR}/contrib/poco)

find_package(ZLIB REQUIRED)
find_package(double-conversion REQUIRED)

# PCRE - bundled regex library in Foundation
file(GLOB POCO_PCRE_SRCS ${POCO_SRC}/Foundation/src/pcre_*.c)
add_library(_poco_foundation_pcre ${POCO_PCRE_SRCS})
add_library(Poco::Foundation::PCRE ALIAS _poco_foundation_pcre)
target_compile_options(_poco_foundation_pcre PRIVATE -w)

# Foundation - exclude platform-specific files that get #included by generic ones
file(GLOB POCO_FOUNDATION_SRCS ${POCO_SRC}/Foundation/src/*.cpp)
list(FILTER POCO_FOUNDATION_SRCS EXCLUDE REGEX
    ".*_(UNIX|POSIX|WIN32|WIN32U|WINCE|VX|DEC|HPUX|QNX|SUN|STD|DUMMY|C99|Android)\\.cpp$|.*(EventLogChannel|WindowsConsoleChannel)\\.cpp$")

add_library(_poco_foundation ${POCO_FOUNDATION_SRCS})
add_library(Poco::Foundation ALIAS _poco_foundation)
target_compile_options(_poco_foundation PRIVATE -w)
target_compile_definitions(_poco_foundation
    PRIVATE
    POCO_UNBUNDLED_ZLIB
    PUBLIC
    POCO_ENABLE_CPP11
    POCO_OS_FAMILY_UNIX
)
target_include_directories(_poco_foundation SYSTEM PUBLIC ${POCO_SRC}/Foundation/include)
target_link_libraries(_poco_foundation PRIVATE
    Poco::Foundation::PCRE
    ZLIB::ZLIB
    double-conversion::double-conversion
)

# Net - patch stream files that use a shared MemoryPool (mutex contention at high concurrency)
# Patches are applied to copies in the build dir so the submodule stays clean.
set(POCO_NET_PATCHED_FILES
    HTTPBufferAllocator
    HTTPFixedLengthStream
    HTTPChunkedStream
    HTTPHeaderStream
    HTTPStream
)

foreach(stem IN LISTS POCO_NET_PATCHED_FILES)
    execute_process(
        COMMAND patch
        -o ${CMAKE_CURRENT_BINARY_DIR}/${stem}.cpp
        ${POCO_SRC}/Net/src/${stem}.cpp
        INPUT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/patches/${stem}.patch
        RESULT_VARIABLE _patch_result
    )
    if(_patch_result GREATER 1)
        message(FATAL_ERROR "Failed to patch ${stem}.cpp (exit code ${_patch_result})")
    endif()
endforeach()

file(GLOB POCO_NET_SRCS ${POCO_SRC}/Net/src/*.cpp)
list(FILTER POCO_NET_SRCS EXCLUDE REGEX
    ".*/Net/src/HTTP(FixedLengthStream|ChunkedStream|HeaderStream|Stream|BufferAllocator)\\.cpp$")
foreach(stem IN LISTS POCO_NET_PATCHED_FILES)
    list(APPEND POCO_NET_SRCS ${CMAKE_CURRENT_BINARY_DIR}/${stem}.cpp)
endforeach()

add_library(_poco_net ${POCO_NET_SRCS})
add_library(Poco::Net ALIAS _poco_net)
target_compile_options(_poco_net PRIVATE -w)
target_compile_definitions(_poco_net PRIVATE POCO_HAVE_FD_EPOLL)
target_include_directories(_poco_net SYSTEM PUBLIC ${POCO_SRC}/Net/include)
target_link_libraries(_poco_net PUBLIC Poco::Foundation)
