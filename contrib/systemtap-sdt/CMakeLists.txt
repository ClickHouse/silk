# Vendored copy of systemtap's <sys/sdt.h> + <sys/sdt-config.h>, the two
# header files needed to emit USDT (Statically Defined Tracepoint) probes.
# Both files are dedicated to the public domain (CC0) by upstream specifically
# so user programs incorporating them are not GPL-tainted; no further attribution
# or LICENSE artifact is required.
#
# We vendor instead of relying on the systemtap-sdt-dev distro package so that
# the silk build has zero extra apt dependencies for USDT.

add_library(systemtap-sdt INTERFACE)
target_include_directories(systemtap-sdt INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
add_library(SystemTap::Sdt ALIAS systemtap-sdt)
