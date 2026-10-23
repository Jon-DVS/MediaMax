#pragma once

// The MediaMax control protocol.
//
// Transport: TCP, one JSON object per line (\n terminated).
// Wire format (stable, versioned):
//
//   Command:  {"v":1,"id":<int>,"method":"<name>","params":{...}}
//   Response: {"v":1,"id":<int>,"ok":true,"result":{...}}
//             {"v":1,"id":<int>,"ok":false,"error":"<message>"}
//
// `id` echoes the request so clients can correlate responses. Requests and
// responses are always 1:1.
//
// Command reference: see docs/PROTOCOL.md.

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace mediamax::protocol
{

inline constexpr int kProtocolVersion = 1;

struct Command
{
    std::int64_t id = 0;
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

struct Response
{
    std::int64_t id = 0;
    bool ok = true;
    nlohmann::json result = nlohmann::json::object();
    std::string error;

    static Response success(std::int64_t id, nlohmann::json result = nlohmann::json::object());
    static Response failure(std::int64_t id, std::string error);
};

// Serializes to a single line (no trailing newline).
std::string encodeCommand(const Command& cmd);
std::string encodeResponse(const Response& resp);

// Parses a line (trailing whitespace/newline tolerated).
bool decodeCommand(const std::string& line, Command& out);
bool decodeResponse(const std::string& line, Response& out);

} // namespace mediamax::protocol
