add_library(robv_vkcore STATIC
    src/buffer.cpp
    src/context.cpp
    src/frame.cpp
    src/image.cpp
    src/shader.cpp
    src/vma.cpp)
target_include_directories(robv_vkcore PUBLIC include)
target_link_libraries(robv_vkcore PUBLIC robv::common robv::log volk::volk vma::vma)
robv_target_defaults(robv_vkcore)

# third party warnings
set_source_files_properties(src/vma.cpp PROPERTIES COMPILE_OPTIONS "-w")

add_library(robv::vkcore ALIAS robv_vkcore)
