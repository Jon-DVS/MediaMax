# MediaMax render module (NVRHI/Vulkan).
#
# Static library: the GPU side of MediaMax. Contains the Vulkan context
# bootstrap, swapchain management and the scene renderer (ping-pong scene
# compositing + per-display output pass).
#
# Shaders (render/shaders/*.vert|frag) are compiled to SPIR-V at build time
# with the SDK's glslangValidator and embedded as C headers
# (cmake/SpirvToHeader.cmake) so the library has no runtime asset loading.

set(MEDIAMAX_RENDER_SHADERS
    layer.vert
    layer.frag
    display.vert
    display.frag)

set(MEDIAMAX_GLSLANG "Vulkan::glslangValidator")
if(NOT TARGET Vulkan::glslangValidator)
    get_filename_component(_vk_loader_dir "${Vulkan_LIBRARY}" DIRECTORY)
    find_program(MEDIAMAX_GLSLANG_EXECUTABLE
        NAMES glslangValidator glslangValidator.exe
        HINTS "${_vk_loader_dir}" "${_vk_loader_dir}/.."
        NO_DEFAULT_PATH)
    if(NOT MEDIAMAX_GLSLANG_EXECUTABLE)
        find_program(MEDIAMAX_GLSLANG_EXECUTABLE NAMES glslangValidator REQUIRED)
    endif()
    set(MEDIAMAX_GLSLANG "${MEDIAMAX_GLSLANG_EXECUTABLE}")
endif()

set(MEDIAMAX_RENDER_SOURCES
    src/VulkanContext.cpp
    src/Swapchain.cpp
    src/SceneRenderer.cpp)

set(MEDIAMAX_RENDER_SHADER_HEADERS "")
foreach(_shader ${MEDIAMAX_RENDER_SHADERS})
    # Name the artifacts <base>_<ext> (layer.vert -> layer_vert): the base
    # name alone would collide for a .vert/.frag pair of the same shader.
    get_filename_component(_base "${_shader}" NAME_WE)
    get_filename_component(_ext "${_shader}" EXT)
    string(REGEX REPLACE "^\\." "" _ext "${_ext}")
    set(_name "${_base}_${_ext}")
    set(_spv "${CMAKE_CURRENT_BINARY_DIR}/shaders/${_name}.spv")
    set(_hdr "${CMAKE_CURRENT_BINARY_DIR}/shaders/${_name}.h")

    add_custom_command(
        OUTPUT "${_spv}"
        COMMAND ${MEDIAMAX_GLSLANG} -V
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}"
                -o "${_spv}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}"
        COMMENT "Compiling ${_shader} to SPIR-V"
        VERBATIM)

    # VERBATIM note: with VERBATIM, source-level quotes in a *concatenated*
    # argument token (e.g. -DIN="${_spv}") are kept as literal quote
    # characters in the generated command line; under the MSVC/cmake.exe
    # path that ends up embedding the quotes into the -D value itself.
    # Plain -DIN=${_spv} is expanded to a bare value and the generator
    # quotes it correctly when the path contains spaces.
    add_custom_command(
        OUTPUT "${_hdr}"
        COMMAND ${CMAKE_COMMAND}
                -DIN=${_spv}
                -DOUT=${_hdr}
                -DNAME=${_name}_spirv
                -P ${CMAKE_SOURCE_DIR}/cmake/SpirvToHeader.cmake
        DEPENDS "${_spv}" "${CMAKE_SOURCE_DIR}/cmake/SpirvToHeader.cmake"
        COMMENT "Embedding ${_name}.spv as a header"
        VERBATIM)

    list(APPEND MEDIAMAX_RENDER_SHADER_HEADERS "${_hdr}")
endforeach()

add_library(mediamax_render STATIC
    ${MEDIAMAX_RENDER_SOURCES}
    ${MEDIAMAX_RENDER_SHADER_HEADERS})
add_library(mediamax::render ALIAS mediamax_render)

target_include_directories(mediamax_render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(mediamax_render
    PUBLIC
        mediamax::mediamax
        nvrhi
        nvrhi_vk
        Vulkan::Vulkan)

mediamax_apply_compiler_settings(mediamax_render)
