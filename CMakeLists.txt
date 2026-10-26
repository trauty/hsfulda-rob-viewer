cmake_minimum_required(VERSION 3.25)

project(rob_viewer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# platform first, the option defaults below depend on it
include(RobvPlatform)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(robv_validation_default ON)
else()
    set(robv_validation_default OFF)
endif()

option(ROBV_BUILD_SERVER "Build robv-server" ON)
option(ROBV_BUILD_CLIENT "Build robv-client" ON)
option(ROBV_BUILD_TOOLS "Build robv-probe" ON)
option(ROBV_WITH_MCAP "MCAP file source (pulls lz4 and zstd)" ON)
option(ROBV_VALIDATION "Enable the Vulkan validation layers by default" ${robv_validation_default})
option(ROBV_USE_SYSTEM_DEPS "find_package() the dependencies instead of fetching them with CPM" OFF)

if(ROBV_PLATFORM_ANDROID OR ROBV_PLATFORM_JETSON)
    set(robv_ffmpeg_default OFF)
else()
    set(robv_ffmpeg_default ON)
endif()
option(ROBV_WITH_FFMPEG "ffmpeg encoder in robv-server and decoder in robv-client (libavcodec)" ${robv_ffmpeg_default})
set(ROBV_ENCODER "null" CACHE STRING "Hardware encoder backend compiled into robv-server next to null")
set_property(CACHE ROBV_ENCODER PROPERTY STRINGS null jetson)
set(ROBV_SANITIZE "" CACHE STRING "Sanitizer for every robv target: address, undefined or thread")

if(ROBV_PLATFORM_ANDROID)
    # Android only runs the client
    set(ROBV_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(ROBV_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
endif()

if(NOT ROBV_ENCODER MATCHES "^(null|jetson)$")
    message(FATAL_ERROR "ROBV_ENCODER must be null or jetson, got '${ROBV_ENCODER}'")
endif()

include(RobvCompileOptions)
# after the dependencies, slangc may come through CPM
include(RobvDependencies)
include(RobvShaders)

add_subdirectory(common)

if(ROBV_BUILD_SERVER OR ROBV_BUILD_CLIENT OR ROBV_BUILD_TOOLS)
    add_subdirectory(vkcore)
endif()

if(ROBV_BUILD_SERVER)
    add_subdirectory(server)
endif()

if(ROBV_BUILD_CLIENT)
    add_subdirectory(client)
endif()

if(ROBV_BUILD_TOOLS)
    add_subdirectory(tools/robv-probe)
endif()

message(STATUS "robv: platform=${ROBV_PLATFORM_NAME} server=${ROBV_BUILD_SERVER} client=${ROBV_BUILD_CLIENT} "
    "tools=${ROBV_BUILD_TOOLS} encoder=${ROBV_ENCODER} ffmpeg=${ROBV_WITH_FFMPEG} mcap=${ROBV_WITH_MCAP}")
