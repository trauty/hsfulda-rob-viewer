# builds a signed APK from a native target with the SDK command line tools, no Gradle
#
#   aapt2 link   manifest + android.jar         to base.apk (no res/, SDL's Java has no R references)
#   javac        SDL's org/libsdl/app/*.java    to classes.jar
#   d8                                          to classes.dex
#   zip          dex deflated, native libs STORED (extractNativeLibs=false maps them in place)
#   zipalign     16 KB pages (Android 15+)
#   apksigner    debug keystore                 to <name>.apk
#
#   robv_add_apk(<native target> NAME <apk name> PACKAGE <id> MANIFEST <AndroidManifest.xml> KEYSTORE <path>)

set(ROBV_ANDROID_SDK "$ENV{ANDROID_HOME}" CACHE PATH "Android SDK root")
if(NOT ROBV_ANDROID_SDK)
    set(ROBV_ANDROID_SDK "$ENV{ANDROID_SDK_ROOT}" CACHE PATH "Android SDK root" FORCE)
endif()
if(NOT IS_DIRECTORY "${ROBV_ANDROID_SDK}")
    message(FATAL_ERROR "Android SDK not found, set ANDROID_HOME or ROBV_ANDROID_SDK")
endif()

set(ROBV_ANDROID_TARGET_SDK 36 CACHE STRING "targetSdk, also the android.jar compiled against")
set(ROBV_ANDROID_MIN_SDK ${ANDROID_PLATFORM_LEVEL})

# newest installed build tools unless pinned
if(NOT ROBV_ANDROID_BUILD_TOOLS)
    file(GLOB build_tools_dirs LIST_DIRECTORIES true "${ROBV_ANDROID_SDK}/build-tools/*")
    list(SORT build_tools_dirs COMPARE NATURAL ORDER DESCENDING)
    list(GET build_tools_dirs 0 newest)
    get_filename_component(newest "${newest}" NAME)
    set(ROBV_ANDROID_BUILD_TOOLS "${newest}" CACHE STRING "build-tools version")
endif()

set(robv_build_tools "${ROBV_ANDROID_SDK}/build-tools/${ROBV_ANDROID_BUILD_TOOLS}")
set(ROBV_ANDROID_JAR "${ROBV_ANDROID_SDK}/platforms/android-${ROBV_ANDROID_TARGET_SDK}/android.jar")
if(NOT EXISTS "${ROBV_ANDROID_JAR}")
    message(FATAL_ERROR "${ROBV_ANDROID_JAR} missing: sdkmanager \"platforms;android-${ROBV_ANDROID_TARGET_SDK}\"")
endif()

foreach(tool aapt2 d8 zipalign apksigner)
    string(TOUPPER "${tool}" var)
    find_program(ROBV_${var} ${tool} PATHS "${robv_build_tools}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
endforeach()

# d8 and apksigner are scripts around jars, they need java on PATH
foreach(tool javac jar keytool java)
    string(TOUPPER "${tool}" var)
    find_program(ROBV_${var} ${tool} HINTS "$ENV{JAVA_HOME}/bin" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
endforeach()

find_program(ROBV_ZIP zip NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(ROBV_ADB adb PATHS "${ROBV_ANDROID_SDK}/platform-tools" NO_CMAKE_FIND_ROOT_PATH)

function(robv_add_apk native_target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAME;PACKAGE;MANIFEST;KEYSTORE" "")

    set(dir "${CMAKE_CURRENT_BINARY_DIR}/apk")
    set(stage "${dir}/stage")
    set(abi_dir "${stage}/lib/${ANDROID_ABI}")
    set(base_apk "${dir}/base.apk")
    set(classes_jar "${dir}/classes.jar")
    set(dex "${stage}/classes.dex")
    set(unaligned_apk "${dir}/unaligned.apk")
    set(aligned_apk "${dir}/aligned.apk")
    set(final_apk "${CMAKE_BINARY_DIR}/${arg_NAME}.apk")

    math(EXPR version_code "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")

    set(java_root "${ROBV_SDL_SOURCE_DIR}/android-project/app/src/main/java")
    file(GLOB java_sources CONFIGURE_DEPENDS "${java_root}/org/libsdl/app/*.java")
    if(NOT java_sources)
        message(FATAL_ERROR "No SDL Java sources under ${java_root}")
    endif()
    list(LENGTH java_sources java_count)

    get_filename_component(java_bin "${ROBV_JAVA}" DIRECTORY)
    set(with_java "${CMAKE_COMMAND}" -E env "PATH=${java_bin}:$ENV{PATH}")

    if(NOT EXISTS "${arg_KEYSTORE}")
        message(STATUS "keytool: generating debug keystore ${arg_KEYSTORE}")
        execute_process(
            COMMAND "${ROBV_KEYTOOL}" -genkeypair -keystore "${arg_KEYSTORE}" -alias androiddebugkey
                    -keyalg RSA -keysize 2048 -validity 10000 -storepass android -keypass android
                    -dname "CN=Android Debug,O=Android,C=US"
            RESULT_VARIABLE keytool_result)
        if(NOT keytool_result EQUAL 0)
            message(FATAL_ERROR "keytool could not generate ${arg_KEYSTORE}")
        endif()
    endif()

    add_custom_command(
        OUTPUT "${base_apk}"
        COMMAND "${ROBV_AAPT2}" link -o "${base_apk}" -I "${ROBV_ANDROID_JAR}" --manifest "${arg_MANIFEST}"
                --min-sdk-version ${ROBV_ANDROID_MIN_SDK} --target-sdk-version ${ROBV_ANDROID_TARGET_SDK}
                --version-code ${version_code} --version-name ${PROJECT_VERSION}
        DEPENDS "${arg_MANIFEST}" "${ROBV_ANDROID_JAR}"
        COMMENT "aapt2 link ${arg_NAME}"
        VERBATIM)

    # android.jar is the boot classpath, so java.* resolves to the device's classes, not the host JDK
    add_custom_command(
        OUTPUT "${classes_jar}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${dir}/classes"
        COMMAND "${ROBV_JAVAC}" -source 8 -target 8 -bootclasspath "${ROBV_ANDROID_JAR}" -Xlint:-options -nowarn
                -encoding utf-8 -d "${dir}/classes" ${java_sources}
        COMMAND "${ROBV_JAR}" cf "${classes_jar}" -C "${dir}/classes" .
        DEPENDS ${java_sources} "${ROBV_ANDROID_JAR}"
        COMMENT "javac SDL Java (${java_count} files)"
        VERBATIM)

    add_custom_command(
        OUTPUT "${dex}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${stage}"
        COMMAND ${with_java} "${ROBV_D8}" --release --min-api ${ROBV_ANDROID_MIN_SDK} --lib "${ROBV_ANDROID_JAR}"
                --output "${stage}" "${classes_jar}"
        DEPENDS "${classes_jar}"
        COMMENT "d8 classes.dex"
        VERBATIM)

    # stripped into the stage, the build dir keeps the symbols for lldb
    add_custom_command(
        OUTPUT "${abi_dir}/libmain.so" "${abi_dir}/libSDL3.so"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${abi_dir}"
        COMMAND "${CMAKE_STRIP}" --strip-unneeded -o "${abi_dir}/libmain.so" "$<TARGET_FILE:${native_target}>"
        COMMAND "${CMAKE_STRIP}" --strip-unneeded -o "${abi_dir}/libSDL3.so" "$<TARGET_FILE:SDL3::SDL3-shared>"
        DEPENDS ${native_target} SDL3::SDL3-shared
        COMMENT "strip native libraries"
        VERBATIM)

    add_custom_command(
        OUTPUT "${unaligned_apk}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${base_apk}" "${unaligned_apk}"
        COMMAND "${ROBV_ZIP}" -q -X "${unaligned_apk}" classes.dex
        COMMAND "${ROBV_ZIP}" -q -X -0 -r "${unaligned_apk}" lib
        WORKING_DIRECTORY "${stage}"
        DEPENDS "${base_apk}" "${dex}" "${abi_dir}/libmain.so" "${abi_dir}/libSDL3.so"
        COMMENT "zip ${arg_NAME}"
        VERBATIM)

    add_custom_command(
        OUTPUT "${aligned_apk}"
        COMMAND "${ROBV_ZIPALIGN}" -P 16 -f 4 "${unaligned_apk}" "${aligned_apk}"
        DEPENDS "${unaligned_apk}"
        COMMENT "zipalign -P 16"
        VERBATIM)

    add_custom_command(
        OUTPUT "${final_apk}"
        COMMAND ${with_java} "${ROBV_APKSIGNER}" sign --ks "${arg_KEYSTORE}" --ks-pass pass:android --key-pass pass:android
                --ks-key-alias androiddebugkey --out "${final_apk}" "${aligned_apk}"
        DEPENDS "${aligned_apk}" "${arg_KEYSTORE}"
        COMMENT "apksigner -> ${final_apk}"
        VERBATIM)

    add_custom_target(apk ALL DEPENDS "${final_apk}")

    if(ROBV_ADB)
        add_custom_target(install-apk
            COMMAND "${ROBV_ADB}" install -r "${final_apk}"
            DEPENDS apk
            COMMENT "adb install ${arg_NAME}.apk"
            VERBATIM)
        add_custom_target(run-apk
            COMMAND "${ROBV_ADB}" shell am start -n "${arg_PACKAGE}/org.libsdl.app.SDLActivity"
            DEPENDS install-apk
            COMMENT "adb shell am start ${arg_PACKAGE}"
            VERBATIM)
    endif()
endfunction()
