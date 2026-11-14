# Root ESP-IDF project file. Order matters — do not reorder.
cmake_minimum_required(VERSION 3.16.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(visioncam)

# --- AGENTS rule 33: exactly one esp32-camera copy ---------------------------
#
# esp32-camera is a PINNED DEPENDENCY (espressif/esp32-camera@2.1.9 declared
# in firmware/src/idf_component.yml, downloaded into managed_components/ and
# locked by dependencies.lock). Rule 33 forbids forking it, vendoring it, or
# ever linking two copies: two copies means two copies of the frame-buffer
# ABI and headers that disagree with the objects actually linked — a failure
# that is silent rather than loud.
#
# This check runs on EVERY configure, so `pio run` aborts before compiling if
# the invariant is broken. It counts registered components that provide the
# driver's public header `esp_camera.h`.
#
# ESP-IDF 6.1.0 itself ships no esp32-camera component (verified by listing
# framework-espidf/components), so framework components cannot legitimately
# provide that header; they are skipped to keep the scan cheap.
idf_build_get_property(__vc_targets BUILD_COMPONENT_TARGETS)
idf_build_get_property(__vc_idf_path IDF_PATH)
string(REGEX REPLACE "/+$" "" __vc_idf_path "${__vc_idf_path}")

set(__vc_camera_copies "")
foreach(__vc_target IN LISTS __vc_targets)
    __component_get_property(__vc_dir ${__vc_target} COMPONENT_DIR)
    if(NOT __vc_dir)
        continue()
    endif()
    file(TO_CMAKE_PATH "${__vc_dir}" __vc_dir)
    string(REGEX REPLACE "/+$" "" __vc_dir "${__vc_dir}")

    # Skip the framework itself — it has no camera driver of its own.
    # Exact prefix match (not a regex): IDF_PATH may contain metacharacters.
    if(__vc_idf_path)
        string(FIND "${__vc_dir}/" "${__vc_idf_path}/" __vc_pos)
        if(__vc_pos EQUAL 0)
            continue()
        endif()
    endif()

    file(GLOB_RECURSE __vc_headers LIST_DIRECTORIES false "${__vc_dir}/*.h")
    list(FILTER __vc_headers INCLUDE REGEX "/esp_camera\\.h$")
    if(__vc_headers)
        list(APPEND __vc_camera_copies "${__vc_dir}")
    endif()
endforeach()

list(LENGTH __vc_camera_copies __vc_camera_count)
if(__vc_camera_count EQUAL 0)
    message(FATAL_ERROR
        "AGENTS rule 33 violated: no registered component provides "
        "esp_camera.h. espressif/esp32-camera@2.1.9 did not resolve — check "
        "firmware/src/idf_component.yml and dependencies.lock.")
elseif(NOT __vc_camera_count EQUAL 1)
    list(JOIN __vc_camera_copies "\n  " __vc_camera_list)
    message(FATAL_ERROR
        "AGENTS rule 33 violated: ${__vc_camera_count} copies of esp32-camera "
        "are registered in this build:\n  ${__vc_camera_list}\n"
        "Exactly one pinned copy may exist (managed_components/ from "
        "firmware/src/idf_component.yml). Remove any vendored or duplicate "
        "copy.")
endif()

list(GET __vc_camera_copies 0 __vc_camera_dir)
message(STATUS "VisionCam: esp32-camera single-copy check OK -> ${__vc_camera_dir}")
