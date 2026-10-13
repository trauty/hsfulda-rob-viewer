# third party dependencies, fetched with CPM or found on the system with ROBV_USE_SYSTEM_DEPS
# each is only pulled in when a target that needs it is built

# one download per source tree, not per preset build dir
if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cpm-cache" CACHE PATH "")
endif()

include(CPM)

set(robv_need_vulkan OFF)
if(ROBV_BUILD_SERVER OR ROBV_BUILD_CLIENT OR ROBV_BUILD_TOOLS)
    set(robv_need_vulkan ON)
endif()

set(robv_need_mcap OFF)
if(ROBV_BUILD_SERVER AND ROBV_WITH_MCAP)
    set(robv_need_mcap ON)
endif()

# spdlog: every binary logs through it, chosen for its android_sink
if(ROBV_USE_SYSTEM_DEPS)
    find_package(spdlog REQUIRED)
else()
    CPMAddPackage(
        NAME spdlog
        GITHUB_REPOSITORY gabime/spdlog
        GIT_TAG v1.17.0
        OPTIONS "SPDLOG_BUILD_EXAMPLE OFF" "SPDLOG_BUILD_TESTS OFF" "SPDLOG_INSTALL OFF")
endif()

if(robv_need_vulkan)
    # headers only, volk opens the loader at runtime, so nothing links libvulkan
    if(ROBV_USE_SYSTEM_DEPS)
        find_package(VulkanHeaders REQUIRED)
    else()
        CPMAddPackage(
            NAME VulkanHeaders
            GITHUB_REPOSITORY KhronosGroup/Vulkan-Headers
            GIT_TAG vulkan-sdk-1.4.357.0)
    endif()

    # volk.c compiles here so the platform defines match, its own CMake would look for a loader
    CPMAddPackage(
        NAME volk
        GITHUB_REPOSITORY zeux/volk
        GIT_TAG vulkan-sdk-1.4.357.0
        DOWNLOAD_ONLY YES)
    add_library(robv_volk STATIC "${volk_SOURCE_DIR}/volk.c")
    target_include_directories(robv_volk SYSTEM PUBLIC "${volk_SOURCE_DIR}")
    target_link_libraries(robv_volk PUBLIC Vulkan::Headers ${CMAKE_DL_LIBS})
    target_compile_definitions(robv_volk PUBLIC VK_NO_PROTOTYPES
        $<$<BOOL:${ROBV_PLATFORM_ANDROID}>:VK_USE_PLATFORM_ANDROID_KHR>)
    add_library(volk::volk ALIAS robv_volk)

    CPMAddPackage(
        NAME VulkanMemoryAllocator
        GITHUB_REPOSITORY GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
        GIT_TAG v3.3.0
        DOWNLOAD_ONLY YES)
    add_library(robv_vma INTERFACE)
    target_include_directories(robv_vma SYSTEM INTERFACE "${VulkanMemoryAllocator_SOURCE_DIR}/include")
    # functions come from volk at runtime, VMA must not reference prototypes
    target_compile_definitions(robv_vma INTERFACE VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
    target_link_libraries(robv_vma INTERFACE volk::volk)
    add_library(vma::vma ALIAS robv_vma)
endif()

if(ROBV_BUILD_SERVER OR ROBV_BUILD_CLIENT)
    if(ROBV_USE_SYSTEM_DEPS)
        find_package(glm REQUIRED)
    else()
        CPMAddPackage(
            NAME glm
            GITHUB_REPOSITORY g-truc/glm
            GIT_TAG 1.0.3
            OPTIONS "GLM_BUILD_LIBRARY OFF" "GLM_BUILD_TESTS OFF" "GLM_BUILD_INSTALL OFF")
    endif()
    add_library(robv_glm INTERFACE)
    target_link_libraries(robv_glm INTERFACE glm::glm)
    target_compile_definitions(robv_glm INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
endif()

if(ROBV_BUILD_CLIENT)
    # fixed path: the APK build compiles SDL's Java from it, a build dir path differs per preset
    if(ROBV_USE_SYSTEM_DEPS AND NOT ROBV_PLATFORM_ANDROID)
        find_package(SDL3 REQUIRED CONFIG)
    else()
        CPMAddPackage(
            NAME SDL3
            GITHUB_REPOSITORY libsdl-org/SDL
            GIT_TAG release-3.4.16
            SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/SDL"
            OPTIONS "SDL_SHARED ON" "SDL_STATIC OFF" "SDL_TEST_LIBRARY OFF" "SDL_TESTS OFF" "SDL_EXAMPLES OFF"
                    "SDL_INSTALL OFF" "SDL_DISABLE_INSTALL ON")
    endif()
    set(ROBV_SDL_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/SDL" CACHE INTERNAL "")

    # no upstream CMakeLists, see third_party/imgui
    CPMAddPackage(
        NAME imgui
        GITHUB_REPOSITORY ocornut/imgui
        GIT_TAG v1.92.9
        DOWNLOAD_ONLY YES)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/imgui" "${CMAKE_BINARY_DIR}/third_party/imgui")
endif()

if(robv_need_mcap)
    if(ROBV_USE_SYSTEM_DEPS)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4>=1.9.3)
        pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd>=1.5.2)
        set(robv_lz4 PkgConfig::LZ4)
        set(robv_zstd PkgConfig::ZSTD)
    else()
        CPMAddPackage(
            NAME lz4
            GITHUB_REPOSITORY lz4/lz4
            GIT_TAG v1.10.0
            SOURCE_SUBDIR build/cmake
            OPTIONS "LZ4_BUILD_CLI OFF" "LZ4_BUILD_LEGACY_LZ4C OFF" "BUILD_SHARED_LIBS OFF" "BUILD_STATIC_LIBS ON")
        CPMAddPackage(
            NAME zstd
            GITHUB_REPOSITORY facebook/zstd
            GIT_TAG v1.5.7
            SOURCE_SUBDIR build/cmake
            OPTIONS "ZSTD_BUILD_PROGRAMS OFF" "ZSTD_BUILD_TESTS OFF" "ZSTD_BUILD_CONTRIB OFF"
                    "ZSTD_BUILD_SHARED OFF" "ZSTD_BUILD_STATIC ON" "ZSTD_LEGACY_SUPPORT OFF")
        set(robv_lz4 lz4_static)
        set(robv_zstd libzstd_static)
    endif()

    # header only, no upstream CMakeLists
    CPMAddPackage(
        NAME mcap
        GITHUB_REPOSITORY foxglove/mcap
        GIT_TAG releases/cpp/v2.1.3
        DOWNLOAD_ONLY YES)
    add_library(robv_mcap INTERFACE)
    target_include_directories(robv_mcap SYSTEM INTERFACE "${mcap_SOURCE_DIR}/cpp/mcap/include")
    target_link_libraries(robv_mcap INTERFACE ${robv_lz4} ${robv_zstd})
    add_library(mcap::mcap ALIAS robv_mcap)
endif()

# the Jetson encoder is not a package: JetPack ships the Multimedia API headers in /usr/src and libs in the tegra dir
if(ROBV_BUILD_SERVER AND ROBV_ENCODER STREQUAL "jetson")
    set(JETSON_MMAPI_ROOT "/usr/src/jetson_multimedia_api" CACHE PATH "Jetson Multimedia API root")
    find_path(JETSON_MMAPI_INCLUDE_DIR NAMES v4l2_nv_extensions.h HINTS "${JETSON_MMAPI_ROOT}/include")
    find_library(JETSON_NVV4L2_LIBRARY NAMES nvv4l2 HINTS /usr/lib/aarch64-linux-gnu/tegra /usr/lib/aarch64-linux-gnu/nvidia)
    find_library(JETSON_NVBUFSURFACE_LIBRARY NAMES nvbufsurface HINTS /usr/lib/aarch64-linux-gnu/tegra /usr/lib/aarch64-linux-gnu/nvidia)
    if(NOT JETSON_MMAPI_INCLUDE_DIR OR NOT JETSON_NVV4L2_LIBRARY OR NOT JETSON_NVBUFSURFACE_LIBRARY)
        message(FATAL_ERROR "ROBV_ENCODER=jetson needs the Jetson Multimedia API (nvidia-l4t-jetson-multimedia-api): "
                            "include=${JETSON_MMAPI_INCLUDE_DIR} nvv4l2=${JETSON_NVV4L2_LIBRARY} "
                            "nvbufsurface=${JETSON_NVBUFSURFACE_LIBRARY}")
    endif()
    add_library(robv_jetson_mmapi INTERFACE)
    target_include_directories(robv_jetson_mmapi SYSTEM INTERFACE "${JETSON_MMAPI_INCLUDE_DIR}")
    target_link_libraries(robv_jetson_mmapi INTERFACE "${JETSON_NVV4L2_LIBRARY}" "${JETSON_NVBUFSURFACE_LIBRARY}")
endif()
