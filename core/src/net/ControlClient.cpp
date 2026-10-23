#include "mediamax/net/ControlClient.h"

namespace mediamax
{

ControlClient::~ControlClient()
{
    disconnect();
}

bool ControlClient::connect(const std::string& host, std::uint16_t port, std::uint32_t timeoutMs)
{
    disconnect();
    return m_Connection.connect(host, port, timeoutMs);
}

void ControlClient::disconnect()
{
    m_Connection.close();
}

bool ControlClient::request(const std::string& method, const nlohmann::json& params,
                            nlohmann::json& outResult, std::string& error, std::uint32_t timeoutMs)
{
    if (!m_Connection.valid())
    {
        error = "not connected";
        return false;
    }

    const protocol::Command cmd{m_NextId++, method, params};
    if (!m_Connection.sendLine(protocol::encodeCommand(cmd)))
    {
        error = "failed to send command";
        disconnect();
        return false;
    }

    std::string line;
    if (!m_Connection.receiveLine(line, timeoutMs))
    {
        error = "no response (timeout or connection lost)";
        disconnect();
        return false;
    }

    protocol::Response resp;
    if (!protocol::decodeResponse(line, resp))
    {
        error = "malformed response: " + line;
        return false;
    }
    if (resp.id != cmd.id)
    {
        error = "response id mismatch";
        return false;
    }
    if (!resp.ok)
    {
        error = resp.error;
        return false;
    }
    outResult = std::move(resp.result);
    return true;
}

bool ControlClient::call(const std::string& method, const nlohmann::json& params, std::string& error,
                         std::uint32_t timeoutMs)
{
    nlohmann::json unused;
    return request(method, params, unused, error, timeoutMs);
}

} // namespace mediamax
