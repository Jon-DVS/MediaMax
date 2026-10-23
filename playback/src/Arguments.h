#pragma once

// Command-line arguments for the playback engine.
//
//   mediamax_playback [--host <ip>] [--port <n>] [--show <file.json>]
//                     [--system-name <name>] [--width <n>] [--height <n>]
//                     [--display WxH@X+Y ...]
//                     [--self-test] [--self-test-out <file.ppm>]
//                     [--self-test-seconds <n>]
//
// --display may be repeated; each occurrence describes one physical output
// (the "wall"). Without --display the app uses a single display of
// --width x --height at (0,0).

#include <cstdint>
#include <string>
#include <vector>

namespace mediamax
{

struct DisplaySpec
{
    int width = 1920;
    int height = 1080;
    int x = 0;
    int y = 0;
};

struct AppArgs
{
    std::string host = "0.0.0.0";
    std::uint16_t port = 47600;
    std::string showPath;
    std::string systemName;
    std::vector<DisplaySpec> displays;
    int width = 1920;
    int height = 1080;

    bool selfTest = false;
    std::string selfTestOut = "selftest.ppm";
    int selfTestSeconds = 3;

    bool help = false;
};

// Parses argv; returns false with `error` set on bad arguments.
bool parseArguments(int argc, char** argv, AppArgs& out, std::string& error);

// Parses "WxH@X+Y" (e.g. "1920x1080@1920+0").
bool parseDisplaySpec(const std::string& spec, DisplaySpec& out, std::string& error);

std::string applicationUsage();

} // namespace mediamax
