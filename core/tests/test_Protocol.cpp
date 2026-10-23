// Control protocol: JSON encode/decode round trips and rejection of malformed
// input. The wire format is one JSON object per line (see protocol/Protocol.h).

#include <doctest/doctest.h>

#include "mediamax/protocol/Protocol.h"

using namespace mediamax;
using namespace mediamax::protocol;

TEST_SUITE("Protocol")
{

TEST_CASE("Command: encode produces a single line with all fields")
{
    Command cmd;
    cmd.id = 42;
    cmd.method = "ping";

    const std::string line = encodeCommand(cmd);
    CHECK(line.find('\n') == std::string::npos);
    CHECK(line.find('\r') == std::string::npos);

    const auto j = nlohmann::json::parse(line);
    CHECK(j.at("v").get<int>() == kProtocolVersion);
    CHECK(j.at("id").get<std::int64_t>() == 42);
    CHECK(j.at("method").get<std::string>() == "ping");
    CHECK(j.at("params").is_object());
    CHECK(j.at("params").empty());
}

TEST_CASE("Command: encode normalizes non-object params to an empty object")
{
    Command cmd;
    cmd.id = 1;
    cmd.method = "test";
    cmd.params = nlohmann::json(123);

    const auto j = nlohmann::json::parse(encodeCommand(cmd));
    CHECK(j.at("params").is_object());
    CHECK(j.at("params").empty());
}

TEST_CASE("Command: decode round trip")
{
    Command cmd;
    cmd.id = 7;
    cmd.method = "playback.seek";
    cmd.params = {{"time", 12.5}, {"label", "test"}};

    Command decoded;
    CHECK(decodeCommand(encodeCommand(cmd), decoded));
    CHECK(decoded.id == 7);
    CHECK(decoded.method == "playback.seek");
    CHECK(decoded.params.at("time").get<double>() == 12.5);
    CHECK(decoded.params.at("label").get<std::string>() == "test");
}

TEST_CASE("Command: decode tolerates surrounding whitespace and CRLF")
{
    Command decoded;
    CHECK(decodeCommand("\t{\"v\":1,\"id\":1,\"method\":\"ping\"}\r\n", decoded));
    CHECK(decoded.method == "ping");
    CHECK(decoded.id == 1);
}

TEST_CASE("Command: decode fills defaults for missing id and params")
{
    Command decoded;
    CHECK(decodeCommand("{\"method\":\"ping\"}", decoded));
    CHECK(decoded.id == 0);
    CHECK(decoded.method == "ping");
    CHECK(decoded.params.is_object());
    CHECK(decoded.params.empty());
}

TEST_CASE("Command: decode rejects malformed input")
{
    Command decoded;
    CHECK_FALSE(decodeCommand("", decoded));
    CHECK_FALSE(decodeCommand("   \r\n", decoded));
    CHECK_FALSE(decodeCommand("not json at all", decoded));
    CHECK_FALSE(decodeCommand("[1,2,3]", decoded)); // array, not object
    CHECK_FALSE(decodeCommand("{\"id\":1}", decoded)); // missing method
    CHECK_FALSE(decodeCommand("{\"method\":42}", decoded)); // method not a string
}

TEST_CASE("Command: decode ignores wrong-typed optional fields")
{
    Command decoded;
    CHECK(decodeCommand("{\"method\":\"ping\",\"id\":\"seven\",\"params\":[1]}", decoded));
    CHECK(decoded.id == 0); // id must be numeric
    CHECK(decoded.params.is_object());
    CHECK(decoded.params.empty()); // params must be an object
}

TEST_CASE("Response: success round trip")
{
    const Response resp = Response::success(9, {{"state", "playing"}, {"time", 1.25}});
    const std::string line = encodeResponse(resp);
    CHECK(line.find('\n') == std::string::npos);

    Response decoded;
    CHECK(decodeResponse(line, decoded));
    CHECK(decoded.id == 9);
    CHECK(decoded.ok);
    CHECK(decoded.error.empty());
    CHECK(decoded.result.at("state").get<std::string>() == "playing");
    CHECK(decoded.result.at("time").get<double>() == 1.25);
}

TEST_CASE("Response: failure round trip")
{
    const Response resp = Response::failure(3, "missing or invalid parameter: path");
    const std::string line = encodeResponse(resp);

    const auto j = nlohmann::json::parse(line);
    CHECK_FALSE(j.contains("result")); // failures never carry a result

    Response decoded;
    CHECK(decodeResponse(line, decoded));
    CHECK(decoded.id == 3);
    CHECK_FALSE(decoded.ok);
    CHECK(decoded.error == "missing or invalid parameter: path");
}

TEST_CASE("Response: decode rejects malformed input")
{
    Response decoded;
    CHECK_FALSE(decodeResponse("", decoded));
    CHECK_FALSE(decodeResponse("garbage", decoded));
    CHECK_FALSE(decodeResponse("{\"method\":\"ping\"}", decoded)); // not a response
    CHECK_FALSE(decodeResponse("{\"ok\":\"true\"}", decoded)); // ok must be boolean
}

TEST_CASE("Response: decode uses a default message when error is missing")
{
    Response decoded;
    CHECK(decodeResponse("{\"v\":1,\"id\":5,\"ok\":false}", decoded));
    CHECK_FALSE(decoded.ok);
    CHECK(!decoded.error.empty());
}

TEST_CASE("Command: encode/decode is stable across repeated round trips")
{
    Command cmd;
    cmd.id = 1000;
    cmd.method = "display.warp.set";
    cmd.params = {
        {"display", 1},
        {"points", {{"0.0", "0.0"}, {"1.0", "0.0"}, {"0.0", "1.0"}, {"1.0", "1.0"}}},
    };

    Command a, b;
    CHECK(decodeCommand(encodeCommand(cmd), a));
    CHECK(decodeCommand(encodeCommand(a), b));
    CHECK(b.id == cmd.id);
    CHECK(b.method == cmd.method);
    CHECK(b.params == cmd.params);
}

} // TEST_SUITE("Protocol")
