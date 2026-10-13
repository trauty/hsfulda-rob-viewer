add_library(robv_server_core STATIC
    src/encoder.cpp
    src/renderer.cpp
    src/session.cpp
    src/source.cpp
    src/transport.cpp)
target_include_directories(robv_server_core PUBLIC include)
target_link_libraries(robv_server_core PUBLIC robv::common robv::log robv::vkcore robv_glm)
robv_add_shaders(robv_server_core FILES shaders/rgb_to_nv12.slang)
robv_target_defaults(robv_server_core)

if(ROBV_ENCODER STREQUAL "jetson")
    target_sources(robv_server_core PRIVATE src/encoder_jetson.cpp)
    target_compile_definitions(robv_server_core PRIVATE ROBV_ENCODER_JETSON=1)
    target_link_libraries(robv_server_core PRIVATE robv_jetson_mmapi)
endif()

if(ROBV_WITH_MCAP)
    # mcap is header only, its implementation compiles in its own translation unit
    target_sources(robv_server_core PRIVATE src/mcap.cpp)
    set_source_files_properties(src/mcap.cpp PROPERTIES COMPILE_OPTIONS "-w")
    target_compile_definitions(robv_server_core PRIVATE ROBV_WITH_MCAP=1)
    target_link_libraries(robv_server_core PRIVATE mcap::mcap)
endif()

add_library(robv::server_core ALIAS robv_server_core)

add_executable(robv-server src/main.cpp)
target_link_libraries(robv-server PRIVATE robv::server_core)
robv_target_defaults(robv-server)
