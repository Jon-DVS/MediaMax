// MediaMax playback engine entry point.
//
//   mediamax_playback [options]        windowed playback (see --help)
//   mediamax_playback --self-test ...  headless self-test (see SelfTest.h)

#include <cstdio>

#include "Arguments.h"
#include "PlaybackApp.h"
#include "SelfTest.h"
#include "mediamax/util/Logger.h"

int main(int argc, char** argv)
{
    mediamax::AppArgs args;
    std::string error;
    if (!mediamax::parseArguments(argc, argv, args, error))
    {
        if (args.help)
        {
            std::printf("%s\n", mediamax::applicationUsage().c_str());
            return 0;
        }
        std::fprintf(stderr, "mediamax_playback: %s\n\n%s\n", error.c_str(),
                     mediamax::applicationUsage().c_str());
        return 2;
    }
    if (args.help)
    {
        std::printf("%s\n", mediamax::applicationUsage().c_str());
        return 0;
    }

    mediamax::Logger::instance().setLevel(mediamax::LogLevel::Info);

    if (args.selfTest)
    {
        return mediamax::runSelfTest(args);
    }

    mediamax::PlaybackApp app(std::move(args));
    return app.run();
}
