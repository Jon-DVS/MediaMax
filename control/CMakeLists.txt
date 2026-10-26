# MediaMax control interface executable.
#
# Links the core (protocol client, show model, file IO) plus the GUI stack:
# Dear ImGui (docking) on GLFW + a minimal raw-Vulkan surface (see
# src/gui/GuiVulkan.cpp). The CLI/REPL modes stay pure console clients; the
# GUI is only built into this executable, never into the core.

add_executable(mediamax_control
    src/main.cpp
    src/gui/ControlGui.cpp
    src/gui/GuiCrash.cpp
    src/gui/GuiMain.cpp
    src/gui/GuiVulkan.cpp
    src/gui/ShowEditor.cpp
    src/gui/SystemConnection.cpp)
add_executable(mediamax::control ALIAS mediamax_control)

target_link_libraries(mediamax_control
    PRIVATE
        mediamax::mediamax
        mediamax::imgui
        glfw
        Vulkan::Vulkan)

target_include_directories(mediamax_control
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
    # MessageBoxW (crash diagnostics) and the same loader deployment as the
    # playback app (no-op on newer SDKs).
    target_link_libraries(mediamax_control PRIVATE user32)
    mediamax_deploy_vulkan_loader(mediamax_control)
endif()

mediamax_apply_compiler_settings(mediamax_control)
