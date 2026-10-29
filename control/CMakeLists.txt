# MediaMax control interface executable.
#
# Links the core (protocol client, show model, file IO) plus the GUI stack:
# Dear ImGui (docking) on GLFW + a minimal raw-Vulkan surface (see
# src/gui/GuiVulkan.cpp). The CLI/REPL modes stay pure console clients; the
# GUI is only built into this executable, never into the core.

# The Open/Save As file dialogs are ImGuiFileDialog (vendored, MIT license -
# see third_party/ImGuiFileDialog/LICENSE). Single-file library, used only by
# the control GUI, never by the core. Like the other third-party targets it
# compiles at the default warning level (the /W4 strictness in
# cmake/CompilerSettings.cmake is for first-party sources only).
set(MM_IMGUI_FILE_DIALOG_DIR ${CMAKE_SOURCE_DIR}/third_party/ImGuiFileDialog)

add_library(imguifiledialog STATIC
    ${MM_IMGUI_FILE_DIALOG_DIR}/ImGuiFileDialog.cpp)
add_library(mediamax::imguifiledialog ALIAS imguifiledialog)
target_compile_features(imguifiledialog PUBLIC cxx_std_20)
target_include_directories(imguifiledialog PUBLIC ${MM_IMGUI_FILE_DIALOG_DIR})
target_link_libraries(imguifiledialog PUBLIC mediamax::imgui)

add_executable(mediamax_control
    src/main.cpp
    src/gui/ControlGui.cpp
    src/gui/GuiCrash.cpp
    src/gui/GuiMain.cpp
    src/gui/GuiVulkan.cpp
    src/gui/ShowEditor.cpp
    src/gui/ShowView.cpp
    src/gui/SystemConnection.cpp)
add_executable(mediamax::control ALIAS mediamax_control)

target_link_libraries(mediamax_control
    PRIVATE
        mediamax::mediamax
        mediamax::imgui
        mediamax::imguifiledialog
        glfw
        Vulkan::Vulkan)

target_include_directories(mediamax_control
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The project version (MEDIAMAX_VERSION) comes in PUBLIC from
# mediamax::mediamax; the GUI shows it in Help > About.

if(WIN32)
    # MessageBoxW (crash diagnostics), DwmSetWindowAttribute (dark title
    # bar) and the same loader deployment as the playback app (no-op on
    # newer SDKs).
    target_link_libraries(mediamax_control PRIVATE user32 dwmapi)
    mediamax_deploy_vulkan_loader(mediamax_control)
endif()

mediamax_apply_compiler_settings(mediamax_control)
