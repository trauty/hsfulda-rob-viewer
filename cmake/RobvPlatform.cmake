# which platform this configure targets, and the one place that knows how an app differs per platform

set(ROBV_PLATFORM_ANDROID OFF)
set(ROBV_PLATFORM_LINUX OFF)
set(ROBV_PLATFORM_JETSON OFF)

if(ANDROID)
    set(ROBV_PLATFORM_ANDROID ON)
    set(ROBV_PLATFORM_NAME "android-${ANDROID_ABI}")
    if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "Only arm64-v8a is supported, got ${ANDROID_ABI}")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ROBV_PLATFORM_LINUX ON)
    set(ROBV_PLATFORM_NAME "linux-${CMAKE_SYSTEM_PROCESSOR}")
    # JetPack writes this file, the cheapest reliable Jetson test
    if(EXISTS "/etc/nv_tegra_release" AND NOT CMAKE_CROSSCOMPILING)
        set(ROBV_PLATFORM_JETSON ON)
        set(ROBV_PLATFORM_NAME "jetson-${CMAKE_SYSTEM_PROCESSOR}")
    endif()
else()
    message(FATAL_ERROR "Unsupported platform ${CMAKE_SYSTEM_NAME}, robv builds for Linux and Android")
endif()

# an app is an executable everywhere except Android, where SDLActivity loads it as libmain.so
function(robv_add_app target)
    if(ROBV_PLATFORM_ANDROID)
        add_library(${target} SHARED ${ARGN})
        set_target_properties(${target} PROPERTIES OUTPUT_NAME main)
    else()
        add_executable(${target} ${ARGN})
    endif()
endfunction()
