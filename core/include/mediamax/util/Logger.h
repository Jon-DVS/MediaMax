#pragma once

// Minimal thread-safe logger. Writes leveled, timestamped messages to stderr.
//
// The logger is intentionally dependency-free and safe to call from any
// thread. Message construction uses std::format syntax but is formatted at
// runtime (vformat), so format strings may be composed dynamically and the
// call never participates in compile-time format-string checks.

#include <atomic>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace mediamax
{

enum class LogLevel : int
{
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
};

class Logger
{
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    LogLevel level() const { return m_Level.load(); }

    template <typename... Args>
    void debug(std::string_view fmt, Args&&... args)
    {
        logf(LogLevel::Debug, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(std::string_view fmt, Args&&... args)
    {
        logf(LogLevel::Info, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warning(std::string_view fmt, Args&&... args)
    {
        logf(LogLevel::Warning, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(std::string_view fmt, Args&&... args)
    {
        logf(LogLevel::Error, fmt, std::forward<Args>(args)...);
    }

private:
    // Runtime-only formatting: std::vformat takes a plain string_view, so
    // there is no format_string involved (and no compile-time checking).
    // Arguments are passed by const lvalue reference (not forwarded) to work
    // around an MSVC deduction bug in make_format_args with forwarding packs.
    template <typename... Args>
    void logf(LogLevel level, std::string_view fmt, const Args&... args)
    {
        if (level < m_Level.load(std::memory_order_relaxed))
        {
            return;
        }
        const auto formatArgs = std::make_format_args(args...);
        log(level, std::vformat(fmt, formatArgs));
    }

    void log(LogLevel level, const std::string& message);

    // Relaxed atomic: the level is read on the hot path without holding
    // m_Mutex, so a torn read must not be possible even though setLevel
    // writes it.
    std::atomic<LogLevel> m_Level{LogLevel::Info};
    std::mutex m_Mutex;
};

} // namespace mediamax
