# executable on Linux, libmain.so inside an APK on Android (see robv_add_app)
robv_add_app(robv-client
    src/app.cpp
    src/decoder.cpp
    src/main.cpp
    src/net.cpp
    src/presenter.cpp
    src/video_view.cpp
    src/vsync.cpp)
target_include_directories(robv-client PRIVATE include)
target_link_libraries(robv-client PRIVATE robv::common robv::log robv::vkcore imgui::imgui SDL3::SDL3)
robv_add_shaders(robv-client FILES shaders/video.slang)
robv_target_defaults(robv-client)

if(ROBV_WITH_FFMPEG AND NOT ROBV_PLATFORM_ANDROID)
    target_sources(robv-client PRIVATE src/decoder_ffmpeg.cpp)
    target_compile_definitions(robv-client PRIVATE ROBV_DECODER_FFMPEG=1)
    target_link_libraries(robv-client PRIVATE PkgConfig::FFMPEG)
endif()

if(ROBV_PLATFORM_ANDROID)
    target_sources(robv-client PRIVATE src/decoder_mediacodec.cpp)
    target_compile_definitions(robv-client PRIVATE ROBV_DECODER_MEDIACODEC=1)
    target_link_libraries(robv-client PRIVATE android log mediandk)

    include(RobvAndroidApk)
    robv_add_apk(robv-client
        NAME robv-client
        PACKAGE de.hsfulda.robv
        MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/android/AndroidManifest.xml"
        KEYSTORE "${CMAKE_CURRENT_SOURCE_DIR}/android/debug.keystore")
else()
    # libSDL3.so sits next to the binary in the build tree
    set_target_properties(robv-client PROPERTIES BUILD_RPATH "$<TARGET_FILE_DIR:SDL3::SDL3-shared>")
endif()
