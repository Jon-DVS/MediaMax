// MediaMax control interface.
//
//   mediamax_control                              (windowed control GUI)
//   mediamax_control --show <file> --list
//   mediamax_control --show <file> --system <name|idx> --command "method" [json-params]
//   mediamax_control --show <file> --system <name|idx>          (interactive REPL)
//   mediamax_control --system <host:port>                       (direct, no file)
//   mediamax_control --gui                        (explicit GUI; = no args)
//   mediamax_control --gui-self-test [frames]     (headless GUI smoke test)
//       [--gui-self-test-system host:port]        (also drive a live engine)
//
// The system list lives in the show file (its "systems" field, issue 18):
// --show <file> makes --system <name|idx> resolve from it; a direct
// --system host:port works without any file.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gui/GuiMain.h"
#include "mediamax/net/ControlClient.h"
#include "mediamax/protocol/CommandServer.h"
#include "mediamax/show/Show.h"
#include "mediamax/util/Logger.h"

namespace
{

using json = nlohmann::json;

struct System
{
    std::string name;
    std::string host;
    std::uint16_t port = 47600;
};

struct Config
{
    std::string path;
    std::vector<System> systems;
};

// Reads the system list from a show file (issue 18: the show document is
// where the systems live). The core show parser validates the file, so a
// broken show is reported as one.
bool loadShowSystems(const std::string& path, Config& out, std::string& error)
{
    std::ifstream in(path);
    if (!in)
    {
        error = "cannot read " + path;
        return false;
    }
    const json parsed = json::parse(in, nullptr, false);
    if (parsed.is_discarded())
    {
        error = "invalid JSON in " + path;
        return false;
    }
    auto show = mediamax::Show::fromJson(parsed, error);
    if (!show)
    {
        return false;
    }
    out.path = path;
    out.systems.clear();
    for (const auto& s : show->systems)
    {
        System sys;
        sys.name = s.name;
        sys.host = s.host;
        sys.port = s.port;
        out.systems.push_back(std::move(sys));
    }
    return true;
}

// Resolves --system: a config name, a 1-based index, or host:port.
bool resolveSystem(const std::string& spec, const Config& config, System& out, std::string& error)
{
    const size_t colon = spec.find(':');
    if (colon != std::string::npos)
    {
        out.name = spec;
        out.host = spec.substr(0, colon);
        try
        {
            const int port = std::stoi(spec.substr(colon + 1));
            if (port < 1 || port > 65535)
            {
                throw std::invalid_argument("port");
            }
            out.port = static_cast<std::uint16_t>(port);
        }
        catch (const std::exception&)
        {
            error = "bad host:port: " + spec;
            return false;
        }
        return true;
    }

    if (config.systems.empty())
    {
        error = "no systems in the show file (use --show <file> or host:port)";
        return false;
    }
    // Index?
    try
    {
        const int idx = std::stoi(spec);
        if (idx >= 1 && idx <= static_cast<int>(config.systems.size()))
        {
            out = config.systems[idx - 1];
            return true;
        }
        error = "system index out of range: " + spec;
        return false;
    }
    catch (const std::exception&)
    {
        // Fall through to name lookup.
    }
    for (const auto& s : config.systems)
    {
        if (s.name == spec)
        {
            out = s;
            return true;
        }
    }
    error = "unknown system: " + spec;
    return false;
}

void printResult(const json& result)
{
    std::cout << result.dump(2) << std::endl;
}

bool sendCommand(mediamax::ControlClient& client, const std::string& method, const json& params,
                 std::string& error)
{
    json result;
    if (!client.request(method, params, result, error))
    {
        return false;
    }
    printResult(result);
    return true;
}

void printUsage()
{
    std::cout <<
        "MediaMax control interface\n"
        "\n"
        "Usage:\n"
        "  mediamax_control\n"
        "  mediamax_control --gui\n"
        "  mediamax_control --show <file> --list\n"
        "  mediamax_control --show <file> --system <name|idx|host:port> --command \"method\" [json-params]\n"
        "  mediamax_control --show <file> --system <name|idx|host:port>\n"
        "  mediamax_control --gui-self-test [frames] [--gui-self-test-system host:port]\n"
        "\n"
        "Options:\n"
        "  --show <file>      Show file with the system list (--system <name|idx>\n"
        "                     resolves from it)\n"
        "  --list             List the show file's systems and exit\n"
        "  --system <spec>    Target system (show-file name, 1-based index or\n"
        "                     host:port; host:port needs no --show)\n"
        "  --command <m>      One-shot command; optional json-params argument\n"
        "  --gui              Open the windowed control GUI (also the default\n"
        "                     when no other mode is given)\n"
        "  --gui-self-test    Headless GUI smoke test (renders [frames], default 90)\n"
        "  --gui-self-test-system <host:port>\n"
        "                     Also drive a running playback engine through the\n"
        "                     GUI's command path (replaces its loaded show)\n"
        "  -h, --help         This help\n"
        "\n"
        "REPL commands:\n"
        "  system <name|idx|host:port>   Connect to a system\n"
        "  systems                       List the show file's systems\n"
        "  <method> [json-params]        Send a protocol command\n"
        "  help                          This list\n"
        "  quit                          Exit\n";
}

void printReplHelp()
{
    std::cout <<
        "Commands:\n"
        "  system <name|idx|host:port>   Connect to a system\n"
        "  systems                       List the show file's systems\n"
        "  help                          Show this list\n"
        "  quit | exit                   Exit\n"
        "  <method> [json-params]        Send a protocol command\n"
        "    e.g.  ping\n"
        "         status\n"
        "         playback.seek {\"time\": 5.0}\n"
        "         layer.set {\"layer\": 10, \"opacity\": 0.5}\n";
    std::cout << "Protocol methods: ";
    bool first = true;
    for (const auto& m : mediamax::protocol::CommandServer::supportedMethods())
    {
        std::cout << (first ? "" : ", ") << m;
        first = false;
    }
    std::cout << std::endl;
}

bool runRepl(const Config& config, const std::string& initialSystem)
{
    mediamax::ControlClient client;
    System current{};
    bool haveCurrent = false;

    if (!initialSystem.empty())
    {
        std::string error;
        if (resolveSystem(initialSystem, config, current, error) &&
            client.connect(current.host, current.port, 3000))
        {
            haveCurrent = true;
            std::cout << "Connected to " << current.name << " (" << current.host << ":" << current.port << ")"
                      << std::endl;
        }
        else
        {
            std::cout << "Not connected: " << error << std::endl;
        }
    }

    std::string line;
    while (true)
    {
        const std::string prompt = haveCurrent ? (current.name + "> ") : "control> ";
        std::cout << prompt << std::flush;
        if (!std::getline(std::cin, line))
        {
            std::cout << std::endl;
            break;
        }
        // Trim.
        const size_t begin = line.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
        {
            continue;
        }
        const size_t end = line.find_last_not_of(" \t\r\n");
        const std::string trimmed = line.substr(begin, end - begin + 1);

        std::istringstream stream(trimmed);
        std::string word;
        stream >> word;
        const std::string rest = trimmed.substr(word.size());
        const size_t restBegin = rest.find_first_not_of(" \t");
        const std::string arg = restBegin == std::string::npos ? "" : rest.substr(restBegin);

        std::string error;
        if (word == "quit" || word == "exit")
        {
            break;
        }
        else if (word == "help" || word == "?")
        {
            printReplHelp();
        }
        else if (word == "systems" || word == "list")
        {
            if (config.systems.empty())
            {
                std::cout << (config.path.empty()
                                  ? "(no systems given: --show <file> provides them; host:port "
                                    "works without)"
                                  : "(no systems in " + config.path + "; use host:port)")
                          << std::endl;
            }
            for (size_t i = 0; i < config.systems.size(); ++i)
            {
                std::cout << "  " << (i + 1) << ". " << config.systems[i].name << "  "
                          << config.systems[i].host << ":" << config.systems[i].port << std::endl;
            }
        }
        else if (word == "system" || word == "connect")
        {
            if (arg.empty())
            {
                std::cout << "Usage: system <name|idx|host:port>" << std::endl;
                continue;
            }
            System target;
            if (!resolveSystem(arg, config, target, error))
            {
                std::cout << error << std::endl;
                continue;
            }
            client.disconnect();
            if (!client.connect(target.host, target.port, 3000))
            {
                std::cout << "connect to " << target.host << ":" << target.port << " failed" << std::endl;
                haveCurrent = false;
                continue;
            }
            current = target;
            haveCurrent = true;
            std::cout << "Connected to " << target.name << " (" << target.host << ":" << target.port << ")"
                      << std::endl;
        }
        else
        {
            // A protocol method.
            if (!haveCurrent)
            {
                std::cout << "Not connected (system <name|idx|host:port> first)" << std::endl;
                continue;
            }
            json params = json::object();
            if (!arg.empty())
            {
                params = json::parse(arg, nullptr, false);
                if (params.is_discarded())
                {
                    std::cout << "invalid json-params: " << arg << std::endl;
                    continue;
                }
            }
            if (!sendCommand(client, word, params, error))
            {
                std::cout << word << " failed: " << error << std::endl;
            }
        }
    }
    client.disconnect();
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string showPath;
    std::string systemSpec;
    std::string command;
    std::string paramsArg; // optional one-shot json-params (a positional)
    bool listOnly = false;
    bool help = false;
    bool gui = false;
    bool guiSelfTest = false;
    std::string guiSelfTestSystem;
    int guiSelfTestFrames = 90;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
            {
                std::cerr << "missing value for " << a << std::endl;
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help")
        {
            help = true;
        }
        else if (a == "--show")
        {
            showPath = next();
        }
        else if (a == "--list")
        {
            listOnly = true;
        }
        else if (a == "--system")
        {
            systemSpec = next();
        }
        else if (a == "--command")
        {
            command = next();
        }
        else if (a == "--gui")
        {
            gui = true;
        }
        else if (a == "--gui-self-test")
        {
            guiSelfTest = true;
            // Optional frame count as the next token (when numeric).
            if (i + 1 < argc)
            {
                const std::string nextArg = argv[i + 1];
                bool numeric = !nextArg.empty();
                for (const char c : nextArg)
                {
                    if (c < '0' || c > '9')
                    {
                        numeric = false;
                        break;
                    }
                }
                if (numeric)
                {
                    guiSelfTestFrames = std::stoi(nextArg);
                    ++i;
                }
            }
        }
        else if (a == "--gui-self-test-system")
        {
            guiSelfTestSystem = next();
        }
        else if (a.rfind("--", 0) == 0)
        {
            std::cerr << "unknown option: " << a << std::endl;
            std::cerr << "Run 'mediamax_control --help' for usage." << std::endl;
            return 2;
        }
        else
        {
            // The optional json-params of a one-shot command (checked against
            // --command below).
            if (!paramsArg.empty())
            {
                std::cerr << "unexpected extra argument: " << a << std::endl;
                std::cerr << "Run 'mediamax_control --help' for usage." << std::endl;
                return 2;
            }
            paramsArg = a;
        }
    }

    if (help)
    {
        printUsage();
        return 0;
    }

    mediamax::Logger::instance().setLevel(mediamax::LogLevel::Error);

    // GUI modes (the GUI keeps its systems in the show file, issue 18).
    if (gui || guiSelfTest)
    {
        if (gui)
        {
            return mediamax::gui::runGui();
        }
        return mediamax::gui::runGuiSelfTest(guiSelfTestFrames, guiSelfTestSystem);
    }

    Config config;
    std::string error;
    bool configOk = false;
    if (showPath.empty())
    {
        error = "no show file given (--show <file>); --system <name|idx> needs it "
                "(a direct host:port works without one)";
    }
    else
    {
        configOk = loadShowSystems(showPath, config, error);
    }
    if (!configOk)
    {
        if (listOnly)
        {
            std::cerr << "mediamax_control: " << error << std::endl;
            return 1;
        }
        // No usable show file is fine when targeting a direct host:port.
        std::cerr << "note: " << error << std::endl;
    }

    if (listOnly)
    {
        std::cout << "Systems in " << config.path << ":" << std::endl;
        if (config.systems.empty())
        {
            std::cout << "  (none)" << std::endl;
        }
        for (size_t i = 0; i < config.systems.size(); ++i)
        {
            std::cout << "  " << (i + 1) << ". " << config.systems[i].name << "  "
                      << config.systems[i].host << ":" << config.systems[i].port << std::endl;
        }
        return 0;
    }

    if (systemSpec.empty())
    {
        if (command.empty())
        {
            // No CLI mode requested: the windowed GUI is the default mode.
            return mediamax::gui::runGui();
        }
        std::cerr << "mediamax_control: --command requires --system" << std::endl;
        printUsage();
        return 2;
    }

    // A json-params without a command is a usage error.
    if (!paramsArg.empty() && command.empty())
    {
        std::cerr << "mediamax_control: json-params requires --command" << std::endl;
        printUsage();
        return 2;
    }

    // One-shot command.
    if (!command.empty())
    {
        System target;
        if (!resolveSystem(systemSpec, config, target, error))
        {
            std::cerr << "mediamax_control: " << error << std::endl;
            return 1;
        }
        json params = json::object();
        if (!paramsArg.empty())
        {
            params = json::parse(paramsArg, nullptr, false);
            if (params.is_discarded())
            {
                std::cerr << "mediamax_control: invalid json-params: " << paramsArg << std::endl;
                return 1;
            }
        }

        mediamax::ControlClient client;
        if (!client.connect(target.host, target.port, 3000))
        {
            std::cerr << "mediamax_control: connect to " << target.host << ":" << target.port << " failed"
                      << std::endl;
            return 1;
        }
        json result;
        if (!client.request(command, params, result, error))
        {
            std::cerr << "mediamax_control: " << command << " failed: " << error << std::endl;
            client.disconnect();
            return 1;
        }
        printResult(result);
        client.disconnect();
        return 0;
    }

    // Interactive REPL.
    return runRepl(config, systemSpec) ? 0 : 1;
}
