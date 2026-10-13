# executable on Linux, libmain.so inside an APK on Android (see robv_add_app)
robv_add_app(robv-client
    src/app.cpp
    src/decoder.cpp
    src/main.cpp
    src/presenter.cpp
    src/vsync.cpp)
target_include_directories(robv-client PRIVATE include)
target_link_libraries(robv-client PRIVATE robv::common robv::log robv::vkcore imgui::imgui SDL3::SDL3)
robv_target_defaults(robv-client)

if(ROBV_PLATFORM_ANDROID)
    # libandroid, libmediandk and libnativewindow join with the MediaCodec decoder, linked by name
    target_link_libraries(robv-client PRIVATE android log)

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
