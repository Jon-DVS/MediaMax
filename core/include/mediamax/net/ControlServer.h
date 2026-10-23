#pragma once

// The control server: runs the command protocol on a TCP socket.
//
// Architecture:
//   - an accept thread accepts connections while the server is running
//   - each client gets its own thread: receive line -> decode -> dispatch to
//     the handler -> encode response -> send
//   - handler invocations run under a single mutex, so concurrent clients
//     see a consistent view of engine state
//
// Lifetime: client threads capture only shared state (never `this`).
// stop() closes every connection and *joins* the client threads, so it
// returns only after all in-flight commands have completed (a command
// handler that hangs will hold stop() open) and the server can then be
// destroyed immediately.
//
// The server is headless: the app installs a
// std::function<protocol::Response(const protocol::Command&)> (typically
// CommandServer::handle bound to its engine).

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mediamax/net/Tcp.h"
#include "mediamax/protocol/Protocol.h"

namespace mediamax
{

using CommandHandler = std::function<protocol::Response(const protocol::Command&)>;

class ControlServer
{
public:
    ControlServer(std::string host, std::uint16_t port);

    // Stops the server if it is still running (stop is idempotent).
    ~ControlServer() { stop(); }

    // Starts listening and the accept thread. Returns false (with a logged
    // error) if the port cannot be bound.
    bool start();

    // Stops the accept thread, disconnects all clients and joins their
    // threads (in-flight commands finish first). Safe to call from any
    // thread; the server can be destroyed immediately afterwards.
    void stop();

    bool running() const { return m_Running->load(); }

    // The actual bound port (equals the requested port unless 0 was used).
    std::uint16_t port() const { return m_Port; }

    void setHandler(CommandHandler handler);

private:
    void acceptLoop();
    static void serveClient(std::shared_ptr<net::TcpConnection> client,
                            std::shared_ptr<std::atomic<bool>> running,
                            std::shared_ptr<std::mutex> handlerMutex,
                            std::shared_ptr<CommandHandler> handler);

    std::string m_Host;
    std::uint16_t m_Port;

    net::TcpListener m_Listener;
    std::thread m_AcceptThread;

    // Shared with client threads (they must not touch `this`).
    std::shared_ptr<std::atomic<bool>> m_Running = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> m_Stopping = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::mutex> m_HandlerMutex = std::make_shared<std::mutex>();
    std::shared_ptr<CommandHandler> m_Handler = std::make_shared<CommandHandler>();

    // Clients currently being served; closed and joined in stop() for an
    // ordered, prompt shutdown.
    std::mutex m_ClientsMutex;
    std::vector<std::shared_ptr<net::TcpConnection>> m_Clients;
    std::vector<std::thread> m_ClientThreads;
};

} // namespace mediamax
