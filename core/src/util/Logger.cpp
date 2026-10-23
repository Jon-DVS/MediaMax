#include "mediamax/util/Logger.h"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>

namespace mediamax
{

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

void Logger::setLevel(LogLevel level)
{
    m_Level.store(level, std::memory_order_relaxed);
}

void Logger::log(LogLevel level, const std::string& message)
{
    if (level < m_Level.load(std::memory_order_relaxed))
    {
        return;
    }

    const char* tag = "INFO ";
    switch (level)
    {
        case LogLevel::Debug: tag = "DEBUG"; break;
        case LogLevel::Info: tag = "INFO "; break;
        case LogLevel::Warning: tag = "WARN "; break;
        case LogLevel::Error: tag = "ERROR"; break;
    }

    const auto now = std::chrono::system_clock::now();
    const auto sinceEpoch = std::chrono::duration_cast<std::chrono::microseconds>(
                                now.time_since_epoch())
                                .count();
    const std::time_t seconds = static_cast<std::time_t>(sinceEpoch / 1000000);
    const int microseconds = static_cast<int>(sinceEpoch % 1000000);

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif

    char timeBuf[32];
    std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d.%06d", tm.tm_hour, tm.tm_min,
                  tm.tm_sec, microseconds);

    std::lock_guard lock(m_Mutex);
    std::fprintf(stderr, "[%s] [%-5s] %s\n", timeBuf, tag, message.c_str());
    std::fflush(stderr);
}

} // namespace mediamax
