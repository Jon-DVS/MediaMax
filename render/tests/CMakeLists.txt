# MediaMax render tests (GPU smoke + pixel-level checks).
#
# These tests create a headless Vulkan context. On machines without a usable
# GPU/driver every case reports "skipped" (doctest) and the suite still
# passes; on CI machines with a GPU they run full pixel-level checks that
# compare the GPU output against the CPU blend reference.

file(GLOB_RECURSE MEDIAMAX_RENDER_TEST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(mediamax_render_tests ${MEDIAMAX_RENDER_TEST_SOURCES})
target_link_libraries(mediamax_render_tests
    PRIVATE
        mediamax::mediamax
        mediamax::render
        doctest::doctest)

mediamax_apply_compiler_settings(mediamax_render_tests)

# Deploy the Vulkan loader next to the test executable so it resolves without
# relying on the host PATH (a no-op when the SDK installs the loader
# system-wide, which modern SDKs do).
mediamax_deploy_vulkan_loader(mediamax_render_tests)

add_test(NAME mediamax_render_tests COMMAND mediamax_render_tests)
set_tests_properties(mediamax_render_tests PROPERTIES TIMEOUT 300)
