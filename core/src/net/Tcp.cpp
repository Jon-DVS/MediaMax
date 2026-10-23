// TCP implementation over Winsock2 (Windows) or BSD sockets (POSIX).

#include "mediamax/net/Tcp.h"

#include <chrono>
#include <cstring>
#include <stdexcept>

#include "mediamax/util/Logger.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace mediamax::net
{
namespace
{

using SocketHandle = SOCKET;
constexpr SocketHandle kBadSocket = INVALID_SOCKET;

struct WinsockInit
{
    WinsockInit()
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            throw std::runtime_error("WSAStartup failed");
        }
    }
    ~WinsockInit() { WSACleanup(); }
};

void ensureWinsock()
{
    static WinsockInit init;
    (void)init;
}

std::string errorMessage(int code)
{
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(code), 0, buf, sizeof(buf) - 1, nullptr);
    buf[sizeof(buf) - 1] = '\0';
    size_t skip = 0;
    while (skip < sizeof(buf) - 1 && (buf[skip] == '\r' || buf[skip] == '\n' || buf[skip] == ' '))
    {
        ++skip;
    }
    return buf + skip;
}

void closeSocket(SocketHandle handle) { ::closesocket(handle); }

void setNonBlocking(SocketHandle handle, bool nonBlocking)
{
    u_long mode = nonBlocking ? 1UL : 0UL;
    ioctlsocket(handle, FIONBIO, &mode);
}

} // namespace
} // namespace mediamax::net

#else // POSIX

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace mediamax::net
{
namespace
{

using SocketHandle = int;
constexpr SocketHandle kBadSocket = -1;

void ensureWinsock() {}

std::string errorMessage(int code)
{
    return std::string(code == 0 ? "unknown error" : std::strerror(code));
}

void closeSocket(SocketHandle handle) { ::close(handle); }

void setNonBlocking(SocketHandle handle, bool nonBlocking)
{
    const int flags = fcntl(handle, F_GETFL, 0);
    fcntl(handle, F_SETFL, nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

} // namespace
} // namespace mediamax::net

#endif // _WIN32

namespace mediamax::net
{

namespace
{

// Resolves host:port into a list of addresses and connects to the first one
// that accepts, honoring the overall timeout.
bool connectSocket(SocketHandle& outHandle, const std::string& host, std::uint16_t port,
                   std::uint32_t timeoutMs)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string portStr = std::to_string(port);
    addrinfo* result = nullptr;
    if (getaddrinfo(host.empty() ? "127.0.0.1" : host.c_str(), portStr.c_str(), &hints, &result) !=
        0)
    {
        return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    SocketHandle handle = kBadSocket;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next)
    {
        handle = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (handle == kBadSocket)
        {
            continue;
        }

        // Non-blocking connect to honor the timeout portably.
        setNonBlocking(handle, true);

        const int rc = ::connect(handle, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
        if (rc == 0)
        {
            break; // connected
        }
#if defined(_WIN32)
        const int wsaErr = WSAGetLastError();
        const bool inProgress = (wsaErr == WSAEWOULDBLOCK);
#else
        const bool inProgress = (errno == EINPROGRESS);
#endif
        if (inProgress)
        {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(handle, &writeSet);
            const auto now = std::chrono::steady_clock::now();
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (remaining > 0)
            {
                timeval tv;
                tv.tv_sec = static_cast<long>(remaining / 1000);
                tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);
                const int sel = ::select(static_cast<int>(handle) + 1, nullptr, &writeSet, nullptr,
                                         &tv);
                if (sel > 0)
                {
                    int soErr = 0;
                    socklen_t len = sizeof(soErr);
                    getsockopt(handle, SOL_SOCKET, SO_ERROR,
#if defined(_WIN32)
                               reinterpret_cast<char*>(&soErr),
#else
                               &soErr,
#endif
                               &len);
                    if (soErr == 0)
                    {
                        break; // connected
                    }
                }
            }
        }
        closeSocket(handle);
        handle = kBadSocket;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }
    }
    freeaddrinfo(result);

    if (handle != kBadSocket)
    {
        // Restore blocking mode.
        setNonBlocking(handle, false);

        int one = 1;
        setsockopt(handle, IPPROTO_TCP, TCP_NODELAY,
#if defined(_WIN32)
                   reinterpret_cast<const char*>(&one),
#else
                   &one,
#endif
                   sizeof(one));
        outHandle = handle;
        return true;
    }
    return false;
}

} // namespace

TcpConnection::~TcpConnection()
{
    close();
}

TcpConnection::TcpConnection(TcpConnection&& other) noexcept : m_Handle(other.m_Handle)
{
    other.m_Handle = kInvalid;
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_Handle = other.m_Handle;
        other.m_Handle = kInvalid;
    }
    return *this;
}

bool TcpConnection::connect(const std::string& host, std::uint16_t port, std::uint32_t timeoutMs)
{
    ensureWinsock();
    close();
    SocketHandle handle = kBadSocket;
    if (!connectSocket(handle, host, port, timeoutMs))
    {
        Logger::instance().warning("TCP connect to {}:{} failed", host, port);
        return false;
    }
    m_Handle = static_cast<std::intptr_t>(handle);
    return true;
}

bool TcpConnection::sendAll(const void* data, size_t size)
{
    if (m_Handle == kInvalid)
    {
        return false;
    }
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < size)
    {
        const int rc = ::send(static_cast<SocketHandle>(m_Handle), p + sent,
                              static_cast<int>(size - sent), 0);
        if (rc <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

bool TcpConnection::sendLine(const std::string& line)
{
    if (!sendAll(line.data(), line.size()))
    {
        return false;
    }
    const char newline = '\n';
    return sendAll(&newline, 1);
}

bool TcpConnection::receiveLine(std::string& outLine, std::uint32_t timeoutMs)
{
    if (m_Handle == kInvalid)
    {
        return false;
    }
    SocketHandle sock = static_cast<SocketHandle>(m_Handle);

    // select() with an overall deadline instead of SO_RCVTIMEO: the socket
    // option is unreliable on some Windows systems (network filter providers
    // have been observed to ignore it), while select() behaves consistently
    // on both Winsock and BSD sockets.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    outLine.clear();

    // A full line may already be waiting in the buffer from a previous read
    // (one TCP segment can carry several lines).
    for (;;)
    {
        const auto nl = m_Buffer.find('\n');
        if (nl != std::string::npos)
        {
            outLine = m_Buffer.substr(0, nl);
            m_Buffer.erase(0, nl + 1);
            if (!outLine.empty() && outLine.back() == '\r')
            {
                outLine.pop_back();
            }
            return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return false; // no complete line before the deadline
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval tv;
        tv.tv_sec = static_cast<long>(remaining / 1000);
        tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);
        const int sel = ::select(static_cast<int>(sock) + 1, &readSet, nullptr, nullptr, &tv);
        if (sel < 0)
        {
            return false; // error
        }
        if (sel == 0)
        {
            // Timed out. A partial line without a terminator is not a valid
            // protocol message, so report failure (the stream is desynced).
            // The partial data stays buffered for the next call.
            return false;
        }

        char buf[8192];
        const int rc = ::recv(sock, buf, sizeof(buf) - 1, 0);
        if (rc < 0)
        {
            return false; // error
        }
        if (rc == 0)
        {
            return false; // EOF (a truncated line is a protocol error)
        }
        m_Buffer.append(buf, static_cast<size_t>(rc));
        if (m_Buffer.size() > 8 * 1024 * 1024)
        {
            return false; // protocol line too large
        }
    }
}

void TcpConnection::close()
{
    if (m_Handle != kInvalid)
    {
        closeSocket(static_cast<SocketHandle>(m_Handle));
        m_Handle = kInvalid;
    }
    m_Buffer.clear();
}

TcpListener::~TcpListener()
{
    close();
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : m_Handle(other.m_Handle), m_Port(other.m_Port)
{
    other.m_Handle = kInvalid;
    other.m_Port = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_Handle = other.m_Handle;
        m_Port = other.m_Port;
        other.m_Handle = kInvalid;
        other.m_Port = 0;
    }
    return *this;
}

bool TcpListener::bind(const std::string& host, std::uint16_t port)
{
    ensureWinsock();
    close();

    SocketHandle sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kBadSocket)
    {
        return false;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
#if defined(_WIN32)
               reinterpret_cast<const char*>(&reuse),
#else
               &reuse,
#endif
               sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0")
    {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    else if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        // Hostname: resolve.
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
        {
            if (result)
            {
                freeaddrinfo(result);
            }
            closeSocket(sock);
            return false;
        }
        auto* ain = reinterpret_cast<sockaddr_in*>(result->ai_addr);
        addr.sin_addr = ain->sin_addr;
        freeaddrinfo(result);
    }

    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        closeSocket(sock);
        return false;
    }
    if (::listen(sock, 16) != 0)
    {
        closeSocket(sock);
        return false;
    }

    // Read back the actual port (matters when binding port 0).
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
    {
        m_Port = ntohs(bound.sin_port);
    }

    m_Handle = static_cast<std::intptr_t>(sock);
    return true;
}

bool TcpListener::accept(TcpConnection& out, std::uint32_t timeoutMs)
{
    if (m_Handle == kInvalid)
    {
        return false;
    }
    SocketHandle sock = static_cast<SocketHandle>(m_Handle);

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);
    timeval tv;
    tv.tv_sec = static_cast<long>(timeoutMs / 1000);
    tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
    const int sel = ::select(static_cast<int>(sock) + 1, &readSet, nullptr, nullptr, &tv);
    if (sel <= 0)
    {
        return false;
    }

    SocketHandle client = ::accept(sock, nullptr, nullptr);
    if (client == kBadSocket)
    {
        return false;
    }

    int one = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
#if defined(_WIN32)
               reinterpret_cast<const char*>(&one),
#else
               &one,
#endif
               sizeof(one));

    out = TcpConnection();
    out.m_Handle = static_cast<std::intptr_t>(client);
    return true;
}

void TcpListener::close()
{
    if (m_Handle != kInvalid)
    {
        closeSocket(static_cast<SocketHandle>(m_Handle));
        m_Handle = kInvalid;
        m_Port = 0;
    }
}

} // namespace mediamax::net
