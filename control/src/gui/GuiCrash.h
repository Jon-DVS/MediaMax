#pragma once

// Unhandled-exception diagnostics for the control GUI (see GuiCrash.cpp).
//
// The GUI used to die on an unhandled exception with no explanation ("the
// window just closes"). installCrashDiagnostics() installs an
// unhandled-exception filter on the calling thread that writes a crash
// report (registers + a full Dear ImGui state dump: every window and every
// dock node with flags, pointers and draw-list splitter state) to
// `mediamax_control_crash.log` next to the executable, and shows a message
// box, so a failure is never silent again.

#include <string>

namespace mediamax::gui
{

// Installs the crash report filter. Must be called from the thread that
// owns the GUI (before the frame loop); a no-op when it cannot be
// installed or on non-Windows platforms. Safe to call more than once.
void installCrashDiagnostics();

// Appends a timestamped line to the same crash/diagnostic log file. Used
// by the per-frame dock invariant checks so that any drift is recorded
// even when the process does not crash. Never throws; ignores failures.
void appendDiagnostic(const std::string& line);

} // namespace mediamax::gui
