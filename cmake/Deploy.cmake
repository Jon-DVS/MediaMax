# Single-output deployment (issue 17): the `mediamax_dist` target collects
# everything needed to run the final output into one folder
# (build/dist/):
#
#   - mediamax_playback.exe and mediamax_control.exe
#   - their runtime DLLs: the FFmpeg shared build's DLLs (playback) and the
#     Vulkan loader (when the SDK ships one; see VulkanDeploy.cmake)
#   - the example shows (the content the engine plays and the GUI opens)
#
# so the folder can be copied to another machine and run as-is:
#
#   cmake --build build --config Release --target mediamax_dist
#   build\dist\mediamax_playback.exe --show examples\01_basics\show.json
#
# The folder is rebuilt (removed and repopulated) on every run of the
# target, like any other build output.

set(MEDIAMAX_DIST_DIR "${CMAKE_BINARY_DIR}/dist")

# The runtime files of the two executables. The FFmpeg DLLs are a
# configure-time list (FFmpeg.cmake); the loader path is empty when the SDK
# installs it system-wide; the executables are generator expressions so
# each configuration deploys its own build.
set(_dist_files
    "$<TARGET_FILE:mediamax_playback>"
    "$<TARGET_FILE:mediamax_control>")
foreach(_ffmpeg_dll IN LISTS MEDIAMAX_FFMPEG_DLLS)
    list(APPEND _dist_files "${_ffmpeg_dll}")
endforeach()
if(MEDIAMAX_VULKAN_LOADER)
    list(APPEND _dist_files "${MEDIAMAX_VULKAN_LOADER}")
endif()

add_custom_target(mediamax_dist
    DEPENDS mediamax_playback mediamax_control
    COMMAND ${CMAKE_COMMAND} -E rm -r -f "${MEDIAMAX_DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MEDIAMAX_DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy ${_dist_files} "${MEDIAMAX_DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/examples" "${MEDIAMAX_DIST_DIR}/examples"
    COMMENT "Collecting the final output (executables, runtime DLLs, examples) into ${MEDIAMAX_DIST_DIR}"
    VERBATIM)
