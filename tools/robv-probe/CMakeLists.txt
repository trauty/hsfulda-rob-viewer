add_executable(robv-probe main.cpp)
target_link_libraries(robv-probe PRIVATE robv::vkcore robv::log)

target_compile_definitions(robv-probe PRIVATE ROBV_PROBE_ENCODERS="null$<$<STREQUAL:${ROBV_ENCODER},jetson>:, jetson>")
robv_target_defaults(robv-probe)
