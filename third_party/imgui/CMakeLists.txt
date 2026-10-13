set(src "${imgui_SOURCE_DIR}")

add_library(robv_imgui STATIC
    "${src}/imgui.cpp"
    "${src}/imgui_demo.cpp"
    "${src}/imgui_draw.cpp"
    "${src}/imgui_tables.cpp"
    "${src}/imgui_widgets.cpp"
    "${src}/backends/imgui_impl_sdl3.cpp"
    "${src}/backends/imgui_impl_vulkan.cpp")

target_include_directories(robv_imgui SYSTEM PUBLIC "${src}" "${src}/backends")
target_link_libraries(robv_imgui PUBLIC SDL3::SDL3 volk::volk)
# the Vulkan backend calls through volk's pointers
target_compile_definitions(robv_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

add_library(imgui::imgui ALIAS robv_imgui)
