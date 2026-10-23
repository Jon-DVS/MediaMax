#include "Arguments.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace mediamax
{

bool parseDisplaySpec(const std::string& spec, DisplaySpec& out, std::string& error)
{
    // WxH@X+Y
    const size_t at = spec.find('@');
    if (at == std::string::npos)
    {
        error = "display spec must be WxH@X+Y";
        return false;
    }
    const std::string wh = spec.substr(0, at);
    const std::string xy = spec.substr(at + 1);
    const size_t x = wh.find('x');
    if (x == std::string::npos)
    {
        error = "display size must be WxH";
        return false;
    }
    try
    {
        out.width = std::stoi(wh.substr(0, x));
        out.height = std::stoi(wh.substr(x + 1));
    }
    catch (const std::exception&)
    {
        error = "bad display size: " + wh;
        return false;
    }

    // X+Y: the separator is the last '+' (X may be negative).
    const size_t plus = xy.rfind('+');
    if (plus == std::string::npos)
    {
        error = "display position must be X+Y";
        return false;
    }
    try
    {
        out.x = std::stoi(xy.substr(0, plus));
        out.y = std::stoi(xy.substr(plus + 1));
    }
    catch (const std::exception&)
    {
        error = "bad display position: " + xy;
        return false;
    }

    if (out.width <= 0 || out.height <= 0)
    {
        error = "display size must be positive";
        return false;
    }
    return true;
}

bool parseArguments(int argc, char** argv, AppArgs& out, std::string& error)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&](const char* opt) -> std::string {
            if (i + 1 >= argc)
            {
                error = std::string("missing value for ") + opt;
                throw std::runtime_error(error);
            }
            return argv[++i];
        };
        try
        {
            if (a == "-h" || a == "--help")
            {
                out.help = true;
            }
            else if (a == "--host")
            {
                out.host = next("--host");
            }
            else if (a == "--port")
            {
                const int p = std::stoi(next("--port"));
                if (p < 0 || p > 65535)
                {
                    error = "port out of range";
                    return false;
                }
                out.port = static_cast<std::uint16_t>(p);
            }
            else if (a == "--show")
            {
                out.showPath = next("--show");
            }
            else if (a == "--system-name")
            {
                out.systemName = next("--system-name");
            }
            else if (a == "--width")
            {
                out.width = std::stoi(next("--width"));
            }
            else if (a == "--height")
            {
                out.height = std::stoi(next("--height"));
            }
            else if (a == "--display")
            {
                DisplaySpec spec;
                if (!parseDisplaySpec(next("--display"), spec, error))
                {
                    return false;
                }
                out.displays.push_back(spec);
            }
            else if (a == "--self-test")
            {
                out.selfTest = true;
            }
            else if (a == "--self-test-out")
            {
                out.selfTestOut = next("--self-test-out");
            }
            else if (a == "--self-test-seconds")
            {
                out.selfTestSeconds = std::stoi(next("--self-test-seconds"));
            }
            else
            {
                error = "unknown argument: " + a;
                return false;
            }
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    if (out.width <= 0 || out.height <= 0)
    {
        error = "invalid --width/--height";
        return false;
    }
    return true;
}

std::string applicationUsage()
{
    std::ostringstream os;
    os << "MediaMax playback engine\n"
       << "\n"
       << "Usage: mediamax_playback [options]\n"
       << "\n"
       << "Options:\n"
       << "  --host <ip>            Control server bind address (default 0.0.0.0)\n"
       << "  --port <n>             Control server port (default 47600)\n"
       << "  --show <file.json>     Show file to load at startup\n"
       << "  --system-name <name>   Name reported to the control interface\n"
       << "  --width <n>            Default display width (default 1920)\n"
       << "  --height <n>           Default display height (default 1080)\n"
       << "  --display WxH@X+Y      One physical output; repeat for a wall\n"
       << "  --self-test            Run headless, render offscreen, write a PPM\n"
       << "  --self-test-out <f>    Self-test output PPM (default selftest.ppm)\n"
       << "  --self-test-seconds <n> Self-test duration in seconds (default 3)\n"
       << "  -h, --help             This help\n";
    return os.str();
}

} // namespace mediamax
