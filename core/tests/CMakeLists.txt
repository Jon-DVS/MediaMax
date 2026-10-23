# MediaMax core unit tests (doctest).
#
# A single executable: the core library is fast to link and the whole suite
# runs in well under a second.

file(GLOB_RECURSE MEDIAMAX_CORE_TEST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(mediamax_core_tests ${MEDIAMAX_CORE_TEST_SOURCES})
target_link_libraries(mediamax_core_tests
    PRIVATE
        mediamax::mediamax
        doctest::doctest)

# Directory holding the ffmpeg CLI, used by the FFmpeg tests to generate
# real media files (empty -> those tests report "skipping" and pass).
if(MEDIAMAX_HAS_FFMPEG)
    target_compile_definitions(mediamax_core_tests PRIVATE
        MEDIAMAX_FFMPEG_TOOLS_DIR="${MEDIAMAX_FFMPEG_TOOLS_DIR}")
endif()

mediamax_apply_compiler_settings(mediamax_core_tests)

add_test(NAME mediamax_core_tests COMMAND mediamax_core_tests)
set_tests_properties(mediamax_core_tests PROPERTIES TIMEOUT 300)
