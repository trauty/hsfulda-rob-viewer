# warnings, platform defines and sanitizers for robv's own targets, never third party code

if(ROBV_SANITIZE AND NOT ROBV_SANITIZE MATCHES "^(address|undefined|thread)$")
    message(FATAL_ERROR "ROBV_SANITIZE must be address, undefined or thread, got '${ROBV_SANITIZE}'")
endif()

function(robv_target_defaults target)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY")
        set(scope INTERFACE)
    else()
        set(scope PRIVATE)
    endif()

    target_compile_features(${target} ${scope} cxx_std_20)

    if(NOT type STREQUAL "INTERFACE_LIBRARY")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align -Woverloaded-virtual
            -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough
            # designated initializers leave Vulkan's sType, pNext and flags zeroed on purpose
            -Wno-missing-field-initializers)
    endif()

    target_compile_definitions(${target} ${scope}
        ROBV_VERSION="${PROJECT_VERSION}"
        $<$<BOOL:${ROBV_PLATFORM_ANDROID}>:ROBV_PLATFORM_ANDROID=1>
        $<$<BOOL:${ROBV_PLATFORM_LINUX}>:ROBV_PLATFORM_LINUX=1>
        $<$<BOOL:${ROBV_PLATFORM_JETSON}>:ROBV_PLATFORM_JETSON=1>
        $<$<BOOL:${ROBV_VALIDATION}>:ROBV_VALIDATION_DEFAULT=1>)

    if(ROBV_SANITIZE AND NOT type STREQUAL "INTERFACE_LIBRARY")
        target_compile_options(${target} PRIVATE -fsanitize=${ROBV_SANITIZE} -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=${ROBV_SANITIZE})
    endif()
endfunction()
