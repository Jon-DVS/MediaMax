#pragma once

// Minimal blocking TCP client/server abstraction over BSD sockets (POSIX)
// and Winsock2 (Windows).
//
// This is deliberately tiny: line-oriented, blocking, single-purpose. The
// control protocol (protocol/Protocol.h) runs on top of it.

#include <cstdint>
#include <string>

namespace mediamax::net
{

// A connected TCP socket (or a connected-endpoint wrapper around one).
class TcpConnection
{
    friend class TcpListener;

public:
    TcpConnection() = default;
    ~TcpConnection();

    TcpConnection(TcpConnection&& other) noexcept;
    TcpConnection& operator=(TcpConnection&& other) noexcept;
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    bool valid() const { return m_Handle != kInvalid; }

    // Connects to host:port, waiting up to timeoutMs for the connection.
    bool connect(const std::string& host, std::uint16_t port, std::uint32_t timeoutMs);

    // Sends all bytes. Returns false on error.
    bool sendAll(const void* data, size_t size);
    bool sendLine(const std::string& line);

    // Receives one \n-terminated line (terminator stripped). Waits up to
    // timeoutMs; false on timeout, EOF or error.
    bool receiveLine(std::string& outLine, std::uint32_t timeoutMs);

    void close();

private:
    static constexpr std::intptr_t kInvalid = -1;
    std::intptr_t m_Handle = kInvalid;
    // Bytes already read from the socket but not yet consumed as a full line.
    // TCP segments do not respect line boundaries, so this must persist
    // across receiveLine() calls.
    std::string m_Buffer;
};

// A listening TCP socket.
class TcpListener
{
public:
    TcpListener() = default;
    ~TcpListener();

    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    bool valid() const { return m_Handle != kInvalid; }

    // Binds and listens. host "" = all interfaces. port 0 = ephemeral (use
    // boundPort() afterwards - required for tests).
    bool bind(const std::string& host, std::uint16_t port);

    std::uint16_t boundPort() const { return m_Port; }

    // Accepts a connection, waiting up to timeoutMs. False on timeout/error.
    bool accept(TcpConnection& out, std::uint32_t timeoutMs);

    void close();

private:
    static constexpr std::intptr_t kInvalid = -1;
    std::intptr_t m_Handle = kInvalid;
    std::uint16_t m_Port = 0;
};

} // namespace mediamax::net
