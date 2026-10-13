# colcon package of its own, outside the main CMake tree: the server never links ROS
# build steps in docs/building.md
cmake_minimum_required(VERSION 3.22)
project(robv_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)

# robv::common has no dependencies, so its sources compile straight in
# the symlink colcon follows still resolves to the repository
get_filename_component(robv_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." REALPATH)
set(ROBV_COMMON_DIR "${robv_root}/common" CACHE PATH "robv common/ directory")
if(NOT EXISTS "${ROBV_COMMON_DIR}/include/robv/common/protocol.h")
    message(FATAL_ERROR "robv common/ not found at ${ROBV_COMMON_DIR}, set ROBV_COMMON_DIR")
endif()

add_library(robv_bridge_common STATIC
    "${ROBV_COMMON_DIR}/src/point_cloud.cpp"
    "${ROBV_COMMON_DIR}/src/protocol.cpp"
    "${ROBV_COMMON_DIR}/src/uri.cpp"
    "${ROBV_COMMON_DIR}/src/video.cpp"
    "${ROBV_COMMON_DIR}/src/view_state.cpp"
    src/socket_sender.cpp)
target_include_directories(robv_bridge_common PUBLIC "${ROBV_COMMON_DIR}/include" src)
set_target_properties(robv_bridge_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(robv_bridge src/bridge_node.cpp)
target_link_libraries(robv_bridge PRIVATE robv_bridge_common)
ament_target_dependencies(robv_bridge rclcpp sensor_msgs)
target_compile_options(robv_bridge PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS robv_bridge DESTINATION lib/${PROJECT_NAME})

ament_package()
