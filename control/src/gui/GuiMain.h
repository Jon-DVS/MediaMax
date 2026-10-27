#pragma once

// Entry points for the control GUI (the --gui and --gui-self-test modes of
// mediamax_control). Owns the GLFW window, the Dear ImGui context and the
// per-frame render loop; the UI state itself lives in ControlGuiApp.

#include <string>

namespace mediamax::gui
{

// Runs the windowed GUI until the window is closed. Returns the process exit
// code (0 on a clean exit). The system list lives in the show file (issue
// 18): File > Open loads it, File > Save writes it back.
int runGui();

// Headless smoke mode: renders `frames` full UI frames to a hidden window and
// exits. When `liveSystem` is non-empty ("host:port"), the scripted command
// sequence (the same path the UI uses) runs first against a running playback
// engine; any failing step makes the self-test fail.
int runGuiSelfTest(int frames, const std::string& liveSystem);

} // namespace mediamax::gui
