# cross compile for the Jetson Orin NX from x86_64, NOT the supported path
# build natively with the Jetson native release preset (docs/building.md)
#
# needs
#   a sysroot copied from the Jetson (JetPack 6, Ubuntu 22.04): /usr/include, /usr/lib, /lib,
#   /usr/src/jetson_multimedia_api (rsync with relative paths works)
#   an aarch64 GCC matching the Jetson's glibc
#   slangc on the host
#
# configure with ROBV_JETSON_SYSROOT, ROBV_ENCODER=jetson, ROBV_BUILD_CLIENT=OFF

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(ROBV_JETSON_SYSROOT "$ENV{ROBV_JETSON_SYSROOT}" CACHE PATH "Root filesystem copied from the Jetson")
if(NOT IS_DIRECTORY "${ROBV_JETSON_SYSROOT}")
    message(FATAL_ERROR "Set ROBV_JETSON_SYSROOT to a sysroot copied from the Jetson, see the comment at the top of ${CMAKE_CURRENT_LIST_FILE}")
endif()

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_SYSROOT "${ROBV_JETSON_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH "${ROBV_JETSON_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Multimedia API libraries live in the tegra dir, found by the dynamic linker only via ld.so.conf.d
set(JETSON_MMAPI_ROOT "${ROBV_JETSON_SYSROOT}/usr/src/jetson_multimedia_api" CACHE PATH "")
link_directories("${ROBV_JETSON_SYSROOT}/usr/lib/aarch64-linux-gnu/tegra")
