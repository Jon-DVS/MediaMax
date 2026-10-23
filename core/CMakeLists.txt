# MediaMax core engine (static library).
#
# Headless: no GPU, no windowing. Everything here is deterministically
# unit-testable.

file(GLOB_RECURSE MEDIAMAX_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# FFmpeg support is compiled in only when available.
if(NOT MEDIAMAX_HAS_FFMPEG)
    list(FILTER MEDIAMAX_CORE_SOURCES EXCLUDE REGEX ".*/FFmpegSource\\.cpp$")
endif()

add_library(mediamax STATIC ${MEDIAMAX_CORE_SOURCES})
add_library(mediamax::mediamax ALIAS mediamax)

target_include_directories(mediamax
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(mediamax
    PUBLIC
        glm
        nlohmann_json::nlohmann_json)

if(MEDIAMAX_HAS_FFMPEG)
    target_compile_definitions(mediamax PUBLIC MEDIAMAX_HAS_FFMPEG=1)
    target_link_libraries(mediamax PUBLIC ${MEDIAMAX_FFMPEG_LIBS})
endif()

mediamax_apply_compiler_settings(mediamax)
