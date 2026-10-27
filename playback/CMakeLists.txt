# MediaMax playback engine executable.
#
# The windowed, multi-display playback app: GLFW windows per physical output,
# the Vulkan/NVRHI render pipeline, the media pump (video+audio decode), the
# miniaudio output device and the control server (network protocol).

set(MEDIAMAX_PLAYBACK_SOURCES
    src/main.cpp
    src/Arguments.cpp
    src/LocalAddress.cpp
    src/MediaPump.cpp
    src/AudioSink.cpp
    src/PlaybackApp.cpp
    src/SelfTest.cpp
    src/WindowSurface.cpp)

add_executable(mediamax_playback ${MEDIAMAX_PLAYBACK_SOURCES})
add_executable(mediamax::playback ALIAS mediamax_playback)

target_link_libraries(mediamax_playback
    PRIVATE
        mediamax::mediamax
        mediamax::render
        glfw
        miniaudio)

target_include_directories(mediamax_playback
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The media pump's FFmpeg path is only available when the core was built with
# FFmpeg; the define comes in PUBLIC from mediamax::mediamax.

mediamax_apply_compiler_settings(mediamax_playback)

# --- Runtime deployment (Windows) ---------------------------------------------
# Copy the FFmpeg runtime DLLs and the Vulkan loader next to the executable so
# it resolves without relying on the host PATH.
if(WIN32)
    if(MEDIAMAX_HAS_FFMPEG)
        add_custom_command(TARGET mediamax_playback POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MEDIAMAX_FFMPEG_DLLS}
                "$<TARGET_FILE_DIR:mediamax_playback>"
            COMMENT "Deploying the FFmpeg runtime DLLs")
    endif()

    mediamax_deploy_vulkan_loader(mediamax_playback)
endif()
