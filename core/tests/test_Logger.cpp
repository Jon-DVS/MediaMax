// Logger: level filtering and the message listener the playback app uses to
// feed the control protocol's log buffer (log.tail).
//
// The Logger is a process-wide singleton, so these tests restore the level
// and clear the listener on the way out (LoggerScope).

#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include "mediamax/util/Logger.h"

using mediamax::Logger;
using mediamax::LogLevel;

namespace
{

struct LoggerScope
{
    LoggerScope()
    {
        oldLevel = Logger::instance().level();
    }
    ~LoggerScope()
    {
        // Convention: a test that installs a listener removes it.
        Logger::instance().setListener(nullptr);
        Logger::instance().setLevel(oldLevel);
    }
    LogLevel oldLevel;
};

} // namespace

TEST_CASE("listener receives every message at or above the level")
{
    LoggerScope scope;
    auto& logger = Logger::instance();
    logger.setLevel(LogLevel::Info);

    std::vector<std::pair<LogLevel, std::string>> seen;
    logger.setListener([&seen](LogLevel level, const std::string& message) {
        seen.push_back({level, message});
    });

    // std::format syntax ({} placeholders), not printf (%d).
    logger.debug("below the level: {}", 1);
    CHECK(seen.empty());

    logger.info("hello {}", 1);
    logger.warning("warn {}", "x");
    logger.error("bad {}", 2);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0].first == LogLevel::Info);
    CHECK(seen[0].second == "hello 1");
    CHECK(seen[1].first == LogLevel::Warning);
    CHECK(seen[1].second == "warn x");
    CHECK(seen[2].first == LogLevel::Error);
    CHECK(seen[2].second == "bad 2");

    // Raising the level filters the listener too (the check happens before
    // anything is emitted).
    logger.setLevel(LogLevel::Error);
    logger.warning("filtered out");
    CHECK(seen.size() == 3);
    logger.error("still in");
    CHECK(seen.size() == 4);
}

TEST_CASE("setListener(nullptr) disables the listener")
{
    LoggerScope scope;
    auto& logger = Logger::instance();
    logger.setLevel(LogLevel::Debug);

    int calls = 0;
    logger.setListener([&calls](LogLevel, const std::string&) { ++calls; });
    logger.info("one");
    CHECK(calls == 1);

    logger.setListener(nullptr);
    logger.info("two");
    CHECK(calls == 1);
}
