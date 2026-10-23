#include "mediamax/protocol/Protocol.h"

#include <optional>

namespace mediamax::protocol
{

Response Response::success(std::int64_t id, nlohmann::json result)
{
    return Response{id, true, std::move(result), {}};
}

Response Response::failure(std::int64_t id, std::string error)
{
    return Response{id, false, nlohmann::json::object(), std::move(error)};
}

std::string encodeCommand(const Command& cmd)
{
    nlohmann::json j;
    j["v"] = kProtocolVersion;
    j["id"] = cmd.id;
    j["method"] = cmd.method;
    j["params"] = cmd.params.is_object() ? cmd.params : nlohmann::json::object();
    return j.dump();
}

std::string encodeResponse(const Response& resp)
{
    nlohmann::json j;
    j["v"] = kProtocolVersion;
    j["id"] = resp.id;
    j["ok"] = resp.ok;
    if (resp.ok)
    {
        j["result"] = resp.result.is_object() ? resp.result : nlohmann::json::object();
    }
    else
    {
        j["error"] = resp.error;
    }
    return j.dump();
}

namespace
{

std::string trim(const std::string& s)
{
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
    {
        return {};
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

} // namespace

bool decodeCommand(const std::string& line, Command& out)
{
    const std::string trimmed = trim(line);
    if (trimmed.empty())
    {
        return false;
    }
    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(trimmed);
    }
    catch (const nlohmann::json::exception&)
    {
        return false;
    }
    if (!j.is_object() || !j.contains("method") || !j["method"].is_string())
    {
        return false;
    }
    out = Command{};
    out.method = j["method"].get<std::string>();
    if (j.contains("id") && j["id"].is_number())
    {
        out.id = j["id"].get<std::int64_t>();
    }
    if (j.contains("params") && j["params"].is_object())
    {
        out.params = j["params"];
    }
    return true;
}

bool decodeResponse(const std::string& line, Response& out)
{
    const std::string trimmed = trim(line);
    if (trimmed.empty())
    {
        return false;
    }
    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(trimmed);
    }
    catch (const nlohmann::json::exception&)
    {
        return false;
    }
    if (!j.is_object() || !j.contains("ok") || !j["ok"].is_boolean())
    {
        return false;
    }
    out = Response{};
    out.ok = j["ok"].get<bool>();
    if (j.contains("id") && j["id"].is_number())
    {
        out.id = j["id"].get<std::int64_t>();
    }
    if (out.ok)
    {
        if (j.contains("result") && j["result"].is_object())
        {
            out.result = j["result"];
        }
    }
    else
    {
        out.error = j.value("error", std::string("unknown error"));
    }
    return true;
}

} // namespace mediamax::protocol
