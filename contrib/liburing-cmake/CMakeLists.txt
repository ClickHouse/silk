set(LIBURING_SRC ${CMAKE_SOURCE_DIR}/contrib/liburing)
set(LIBURING_BIN ${CMAKE_BINARY_DIR}/liburing-generated)

file(MAKE_DIRECTORY ${LIBURING_BIN})
file(MAKE_DIRECTORY ${LIBURING_BIN}/liburing)

# Extract version from spec file.
file(STRINGS "${LIBURING_SRC}/liburing.spec" _spec_line REGEX "^Version:")
string(REGEX MATCH "[0-9]+\\.[0-9]+" _version "${_spec_line}")
string(REGEX MATCH "^[0-9]+" _version_major "${_version}")
string(REGEX MATCH "[0-9]+$" _version_minor "${_version}")

file(WRITE "${LIBURING_BIN}/liburing/io_uring_version.h"
    "/* SPDX-License-Identifier: MIT */\n"
    "#ifndef LIBURING_VERSION_H\n"
    "#define LIBURING_VERSION_H\n"
    "\n"
    "#define IO_URING_VERSION_MAJOR ${_version_major}\n"
    "#define IO_URING_VERSION_MINOR ${_version_minor}\n"
    "\n"
    "#endif\n"
)

# compat.h: stable for any system capable of running io_uring (kernel >= 5.1).
file(WRITE "${LIBURING_BIN}/liburing/compat.h"
    "/* SPDX-License-Identifier: MIT */\n"
    "#ifndef LIBURING_COMPAT_H\n"
    "#define LIBURING_COMPAT_H\n"
    "\n"
    "#if defined(__has_include)\n"
    "#if __has_include(\"linux/time_types.h\")\n"
    "#include <linux/time_types.h>\n"
    "#else\n"
    "struct __kernel_timespec { int64_t tv_sec; long long tv_nsec; };\n"
    "#endif\n"
    "#define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H 1\n"
    "#endif\n"
    "\n"
    "#if !defined(__has_include)\n"
    "#include <linux/time_types.h>\n"
    "#define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H 1\n"
    "#endif\n"
    "#include <linux/openat2.h>\n"
    "#include <linux/ioctl.h>\n"
    "\n"
    "#ifndef BLOCK_URING_CMD_DISCARD\n"
    "#define BLOCK_URING_CMD_DISCARD _IO(0x12, 0)\n"
    "#endif\n"
    "\n"
    "#endif\n"
)

# config-host.h: all CONFIG_HAVE_* present on any system that supports io_uring.
# CONFIG_NOLIBC is intentionally absent -- we link against libc.
file(WRITE "${LIBURING_BIN}/config-host.h"
    "#define CONFIG_HAVE_KERNEL_RWF_T\n"
    "#define CONFIG_HAVE_KERNEL_TIMESPEC\n"
    "#define CONFIG_HAVE_OPEN_HOW\n"
    "#define CONFIG_HAVE_STATX\n"
    "#define CONFIG_HAVE_GLIBC_STATX\n"
    "#define CONFIG_HAVE_CXX\n"
    "#define CONFIG_HAVE_UCONTEXT\n"
    "#define CONFIG_HAVE_MEMFD_CREATE\n"
    "#define CONFIG_HAVE_NVME_URING\n"
    "#define CONFIG_HAVE_FANOTIFY\n"
    "#define CONFIG_HAVE_FUTEXV\n"
    "#define CONFIG_HAVE_UBLK_HEADER\n"
)

add_library(liburing STATIC
    ${LIBURING_SRC}/src/setup.c
    ${LIBURING_SRC}/src/queue.c
    ${LIBURING_SRC}/src/register.c
    ${LIBURING_SRC}/src/syscall.c
    ${LIBURING_SRC}/src/version.c
)

# LIBURING_BIN must come before LIBURING_SRC/src/include so the generated
# liburing/compat.h and liburing/io_uring_version.h shadow the absent originals.
target_include_directories(liburing SYSTEM PUBLIC ${LIBURING_BIN} ${LIBURING_SRC}/src/include)

target_compile_definitions(liburing PRIVATE
    _GNU_SOURCE
    _LARGEFILE_SOURCE
    _FILE_OFFSET_BITS=64
)

target_compile_options(liburing PRIVATE
    -include ${LIBURING_BIN}/config-host.h
    -Wno-unused-parameter
)

add_library(Liburing::Liburing ALIAS liburing)
