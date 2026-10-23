#pragma once

// The headless self-test: exercises the full pipeline (show, playback engine,
// media pump, scene renderer) without a window or an audio device, renders
// offscreen and writes PPM frames, and verifies the output is not black.
//
//   mediamax_playback --self-test [--self-test-out <base.ppm>]
//                         [--self-test-seconds <n>] [--show <file.json>]
//
// Without --show it runs a built-in demo show (procedural color-bars and
// checkerboard media, no files required).

#include "Arguments.h"

namespace mediamax
{

// Runs the self-test. Returns 0 on success, non-zero on failure.
int runSelfTest(const AppArgs& args);

} // namespace mediamax
