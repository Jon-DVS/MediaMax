// Local address lookup. Kept in its own translation unit because the
// Windows headers it needs (winsock2.h) must not be mixed with the windows.h
// that GLFW pulls in elsewhere (see core/src/net/Tcp.cpp for the same
// pattern).

#include "LocalAddress.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace mediamax
{

namespace
{

#if defined(_WIN32)
// Winsock must be initialized before getaddrinfo. The control server's TCP
// code does this too; a self-contained guard makes the order irrelevant.
struct WinsockInit
{
    WinsockInit()
    {
        WSADATA data;
        (void)WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() { WSACleanup(); }
};

void ensureWinsock()
{
    static WinsockInit init;
    (void)init;
}
#endif

} // namespace

std::string localIpAddress()
{
#if defined(_WIN32)
    ensureWinsock();
#endif

    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0)
    {
        return "127.0.0.1";
    }

    addrinfo hints{};
    hints.ai_family = AF_INET; // IPv4 only
    addrinfo* result = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &result) != 0 || result == nullptr)
    {
        if (result != nullptr)
        {
            freeaddrinfo(result);
        }
        return "127.0.0.1";
    }

    std::string ip;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next)
    {
        if (p->ai_family == AF_INET && p->ai_addr != nullptr)
        {
            char buf[INET_ADDRSTRLEN] = {0};
            const auto* sa = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
            if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf)) != nullptr)
            {
                ip = buf;
                break;
            }
        }
    }
    freeaddrinfo(result);
    return ip.empty() ? std::string("127.0.0.1") : ip;
}

} // namespace mediamax
