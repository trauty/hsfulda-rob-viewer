# robv::common: types, wire protocol, shared view state
# no Vulkan, platform or dependencies, so the ROS 2 bridge compiles it in its own colcon build
add_library(robv_common STATIC
    src/point_cloud.cpp
    src/protocol.cpp
    src/rtp.cpp
    src/uri.cpp
    src/video.cpp
    src/view_state.cpp)
target_include_directories(robv_common PUBLIC include)
robv_target_defaults(robv_common)
add_library(robv::common ALIAS robv_common)

# logging is separate, the one piece that needs spdlog
add_library(robv_log STATIC src/log.cpp)
target_include_directories(robv_log PUBLIC include)
target_link_libraries(robv_log PRIVATE spdlog::spdlog)
if(ROBV_PLATFORM_ANDROID)
    target_link_libraries(robv_log PRIVATE log)
endif()
robv_target_defaults(robv_log)
add_library(robv::log ALIAS robv_log)
