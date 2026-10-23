#pragma once

// The control client: a synchronous request/response client for the control
// protocol. Used by the control interface executable.

#include <cstdint>
#include <string>

#include "mediamax/net/Tcp.h"
#include "mediamax/protocol/Protocol.h"

namespace mediamax
{

class ControlClient
{
public:
    ControlClient() = default;
    ~ControlClient();

    bool connect(const std::string& host, std::uint16_t port, std::uint32_t timeoutMs = 3000);
    void disconnect();
    bool connected() const { return m_Connection.valid(); }

    // Sends a command and waits for the correlated response.
    // Returns false (with error set) on timeout, protocol error or ok=false.
    bool request(const std::string& method, const nlohmann::json& params,
                 nlohmann::json& outResult, std::string& error, std::uint32_t timeoutMs = 5000);

    // Convenience wrapper around request that ignores the result.
    bool call(const std::string& method, const nlohmann::json& params, std::string& error,
              std::uint32_t timeoutMs = 5000);

private:
    net::TcpConnection m_Connection;
    std::int64_t m_NextId = 1;
};

} // namespace mediamax
